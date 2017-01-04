// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include <time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/cbasetypes.h"
#include "../common/core.h"
#include "../common/db.h"
#include "../common/malloc.h"
#include "../common/mapindex.h"
#include "../common/mmo.h"
#include "../common/showmsg.h"
#include "../common/socket.h"
#include "../common/strlib.h"
#include "../common/timer.h"
#include "../common/cli.h"
#include "int_guild.h"
#include "int_homun.h"
#include "int_mercenary.h"
#include "int_elemental.h"
#include "int_party.h"
#include "int_storage.h"
#include "inter.h"
#include "char_logif.h"
#include "char_mapif.h"
#include "char_cnslif.h"
#include "char_clif.h"

//definition of exported var declared in .h
int login_fd=-1; //login file descriptor
int char_fd=-1; //char file descriptor
struct Schema_Config schema_config;
static void char_schema_config_init(void);
static void char_schema_config_final(void);
struct CharServ_Config charserv_config;
struct mmo_map_server map_server[MAX_MAP_SERVERS];
//Custom limits for the fame lists. [Skotlex]
int fame_list_size_chemist = MAX_FAME_LIST;
int fame_list_size_smith = MAX_FAME_LIST;
int fame_list_size_taekwon = MAX_FAME_LIST;
// Char-server-side stored fame lists [DracoRPG]
struct fame_list smith_fame_list[MAX_FAME_LIST];
struct fame_list chemist_fame_list[MAX_FAME_LIST];
struct fame_list taekwon_fame_list[MAX_FAME_LIST];

#define CHAR_MAX_MSG 300	//max number of msg_conf
static char* msg_table[CHAR_MAX_MSG]; // Login Server messages_conf

// check for exit signal
// 0 is saving complete
// other is char_id
unsigned int save_flag = 0;

// Advanced subnet check [LuzZza]
struct s_subnet {
	uint32 mask;
	uint32 char_ip;
	uint32 map_ip;
} subnet[16];
int subnet_count = 0;

int char_chardb_waiting_disconnect(int tid, unsigned int tick, int id, intptr_t data);

DBMap* auth_db; // uint32 account_id -> struct auth_node*
DBMap* online_char_db; // uint32 account_id -> struct online_char_data*
DBMap* char_db_; // uint32 char_id -> struct mmo_charstatus*
DBMap* char_get_authdb() { return auth_db; }
DBMap* char_get_onlinedb() { return online_char_db; }
DBMap* char_get_chardb() { return char_db_; }

/**
 * @see DBCreateData
 */
DBData char_create_online_data(DBKey key, va_list args){
	struct online_char_data* character;
	CREATE(character, struct online_char_data, 1);
	character->account_id = key.i;
	character->char_id = -1;
	character->server = -1;
	character->fd = -1;
	character->waiting_disconnect = INVALID_TIMER;
	return db_ptr2data(character);
}

void char_set_charselect(uint32 account_id) {
	struct online_char_data* character;

	character = (struct online_char_data*)idb_ensure(online_char_db, account_id, char_create_online_data);

	if( character->server > -1 )
		if( map_server[character->server].users > 0 ) // Prevent this value from going negative.
			map_server[character->server].users--;

	character->char_id = -1;
	character->server = -1;

	if(character->waiting_disconnect != INVALID_TIMER) {
		delete_timer(character->waiting_disconnect, char_chardb_waiting_disconnect);
		character->waiting_disconnect = INVALID_TIMER;
	}

	chlogif_send_setacconline(account_id);

}

void char_set_char_online(int map_id, uint32 char_id, uint32 account_id) {
	struct online_char_data* character;
	struct mmo_charstatus *cp;

	//Update DB
	if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `online`='1' WHERE `char_id`='%d' LIMIT 1", charserv_table(char_table), char_id) )
		Sql_ShowDebug(sql_handle);

	//Check to see for online conflicts
	character = (struct online_char_data*)idb_ensure(online_char_db, account_id, char_create_online_data);
	if( character->char_id != -1 && character->server > -1 && character->server != map_id )
	{
		ShowNotice("set_char_online: Character %d:%d marked in map server %d, but map server %d claims to have (%d:%d) online!\n",
			character->account_id, character->char_id, character->server, map_id, account_id, char_id);
		mapif_disconnectplayer(map_server[character->server].fd, character->account_id, character->char_id, 2);
	}

	//Update state data
	character->char_id = char_id;
	character->server = map_id;

	if( character->server > -1 )
		map_server[character->server].users++;

	//Get rid of disconnect timer
	if(character->waiting_disconnect != INVALID_TIMER) {
		delete_timer(character->waiting_disconnect, char_chardb_waiting_disconnect);
		character->waiting_disconnect = INVALID_TIMER;
	}

	//Set char online in guild cache. If char is in memory, use the guild id on it, otherwise seek it.
	cp = (struct mmo_charstatus*)idb_get(char_db_,char_id);
	inter_guild_CharOnline(char_id, cp?cp->guild_id:-1);

	//Notify login server
	chlogif_send_setacconline(account_id);
}

void char_set_char_offline(uint32 char_id, uint32 account_id){
	struct online_char_data* character;

	if ( char_id == -1 )
	{
		if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `online`='0' WHERE `account_id`='%d'", charserv_table(char_table), account_id) )
			Sql_ShowDebug(sql_handle);
	}
	else
	{
		struct mmo_charstatus* cp = (struct mmo_charstatus*)idb_get(char_db_,char_id);
		inter_guild_CharOffline(char_id, cp?cp->guild_id:-1);
		if (cp)
			idb_remove(char_db_,char_id);

		if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `online`='0' WHERE `char_id`='%d' LIMIT 1", charserv_table(char_table), char_id) )
			Sql_ShowDebug(sql_handle);
	}

	if ((character = (struct online_char_data*)idb_get(online_char_db, account_id)) != NULL)
	{	//We don't free yet to avoid aCalloc/aFree spamming during char change. [Skotlex]
		if( character->server > -1 )
			if( map_server[character->server].users > 0 ) // Prevent this value from going negative.
				map_server[character->server].users--;

		if(character->waiting_disconnect != INVALID_TIMER){
			delete_timer(character->waiting_disconnect, char_chardb_waiting_disconnect);
			character->waiting_disconnect = INVALID_TIMER;
		}

		if(character->char_id == char_id)
		{
			character->char_id = -1;
			character->server = -1;
			// needed if player disconnects completely since Skotlex did not want to free the session
			character->pincode_success = false;
		}

		//FIXME? Why Kevin free'd the online information when the char was effectively in the map-server?
	}

	//Remove char if 1- Set all offline, or 2- character is no longer connected to char-server.
	if (char_id == -1 || character == NULL || character->fd == -1){
		chlogif_send_setaccoffline(login_fd,account_id);
	}
}

/**
 * @see DBApply
 */
int char_db_setoffline(DBKey key, DBData *data, va_list ap) {
	struct online_char_data* character = (struct online_char_data*)db_data2ptr(data);
	int server = va_arg(ap, int);
	if (server == -1) {
		character->char_id = -1;
		character->server = -1;
		if(character->waiting_disconnect != INVALID_TIMER){
			delete_timer(character->waiting_disconnect, char_chardb_waiting_disconnect);
			character->waiting_disconnect = INVALID_TIMER;
		}
	} else if (character->server == server)
		character->server = -2; //In some map server that we aren't connected to.
	return 0;
}

/**
 * @see DBApply
 */
static int char_db_kickoffline(DBKey key, DBData *data, va_list ap){
	struct online_char_data* character = (struct online_char_data*)db_data2ptr(data);
	int server_id = va_arg(ap, int);

	if (server_id > -1 && character->server != server_id)
		return 0;

	//Kick out any connected characters, and set them offline as appropriate.
	if (character->server > -1)
		mapif_disconnectplayer(map_server[character->server].fd, character->account_id, character->char_id, 1);
	else if (character->waiting_disconnect == INVALID_TIMER)
		char_set_char_offline(character->char_id, character->account_id);
	else
		return 0; // fail

	return 1;
}

void char_set_all_offline(int id){
	if (id < 0)
		ShowNotice("Sending all users offline.\n");
	else
		ShowNotice("Sending users of map-server %d offline.\n",id);
	online_char_db->foreach(online_char_db,char_db_kickoffline,id);

	if (id >= 0 || !chlogif_isconnected())
		return;
	//Tell login-server to also mark all our characters as offline.
	chlogif_send_setallaccoffline(login_fd);
}

void char_set_all_offline_sql(void){
	//Set all players to 'OFFLINE'
	if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `online` = '0'", charserv_table(char_table)) )
		Sql_ShowDebug(sql_handle);
	if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `online` = '0'", charserv_table(guild_member_table)) )
		Sql_ShowDebug(sql_handle);
	if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `connect_member` = '0'", charserv_table(guild_table)) )
		Sql_ShowDebug(sql_handle);
}

/**
 * @see DBCreateData
 */
static DBData char_create_charstatus(DBKey key, va_list args) {
	struct mmo_charstatus *cp;
	cp = (struct mmo_charstatus *) aCalloc(1,sizeof(struct mmo_charstatus));
	cp->char_id = key.i;
	return db_ptr2data(cp);
}

int char_inventory_to_sql(const struct item items[], int max, int id);

int char_mmo_char_tosql(uint32 char_id, struct mmo_charstatus* p){
	int i = 0;
	int count = 0;
	int diff = 0;
	char save_status[128]; //For displaying save information. [Skotlex]
	struct mmo_charstatus *cp;
	int errors = 0; //If there are any errors while saving, "cp" will not be updated at the end.
	StringBuf buf;

	if (char_id!=p->char_id) return 0;

	cp = (struct mmo_charstatus *)idb_ensure(char_db_, char_id, char_create_charstatus);

	StringBuf_Init(&buf);
	memset(save_status, 0, sizeof(save_status));

	//map inventory data
	if( memcmp(p->inventory, cp->inventory, sizeof(p->inventory)) ) {
		if (!char_inventory_to_sql(p->inventory, MAX_INVENTORY, p->char_id))
			strcat(save_status, " inventory");
		else
			errors++;
	}

	//map cart data
	if( memcmp(p->cart, cp->cart, sizeof(p->cart)) ) {
		if (!char_memitemdata_to_sql(p->cart, MAX_CART, p->char_id, TABLE_CART))
			strcat(save_status, " cart");
		else
			errors++;
	}

	//map storage data
	if( memcmp(p->storage.items, cp->storage.items, sizeof(p->storage.items)) ) {
		if (!char_memitemdata_to_sql(p->storage.items, MAX_STORAGE, p->account_id, TABLE_STORAGE))
			strcat(save_status, " storage");
		else
			errors++;
	}

	if (
		(p->base_exp != cp->base_exp) || (p->base_level != cp->base_level) ||
		(p->job_level != cp->job_level) || (p->job_exp != cp->job_exp) ||
		(p->zeny != cp->zeny) ||
		(p->last_point.map != cp->last_point.map) ||
		(p->last_point.x != cp->last_point.x) || (p->last_point.y != cp->last_point.y) ||
		(p->max_hp != cp->max_hp) || (p->hp != cp->hp) ||
		(p->max_sp != cp->max_sp) || (p->sp != cp->sp) ||
		(p->status_point != cp->status_point) || (p->skill_point != cp->skill_point) ||
		(p->str != cp->str) || (p->agi != cp->agi) || (p->vit != cp->vit) ||
		(p->int_ != cp->int_) || (p->dex != cp->dex) || (p->luk != cp->luk) ||
		(p->option != cp->option) ||
		(p->party_id != cp->party_id) || (p->guild_id != cp->guild_id) ||
		(p->pet_id != cp->pet_id) || (p->weapon != cp->weapon) || (p->hom_id != cp->hom_id) ||
		(p->ele_id != cp->ele_id) || (p->shield != cp->shield) || (p->head_top != cp->head_top) ||
		(p->head_mid != cp->head_mid) || (p->head_bottom != cp->head_bottom) || (p->delete_date != cp->delete_date) ||
		(p->rename != cp->rename) || (p->robe != cp->robe) || (p->character_moves != cp->character_moves) ||
		(p->unban_time != cp->unban_time) || (p->font != cp->font) || (p->uniqueitem_counter != cp->uniqueitem_counter) ||
		(p->hotkey_rowshift != cp->hotkey_rowshift)
	)
	{	//Save status
		if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `base_level`='%d', `job_level`='%d',"
			"`base_exp`='%u', `job_exp`='%u', `zeny`='%d',"
			"`max_hp`='%d',`hp`='%d',`max_sp`='%d',`sp`='%d',`status_point`='%d',`skill_point`='%d',"
			"`str`='%d',`agi`='%d',`vit`='%d',`int`='%d',`dex`='%d',`luk`='%d',"
			"`option`='%d',`party_id`='%d',`guild_id`='%d',`pet_id`='%d',`homun_id`='%d',`elemental_id`='%d',"
			"`weapon`='%d',`shield`='%d',`head_top`='%d',`head_mid`='%d',`head_bottom`='%d',"
			"`last_map`='%s',`last_x`='%d',`last_y`='%d',`save_map`='%s',`save_x`='%d',`save_y`='%d', `rename`='%d',"
			"`delete_date`='%lu',`robe`='%d',`moves`='%d',`font`='%u',`uniqueitem_counter`='%u',"
			"`hotkey_rowshift`='%d'"
			" WHERE `account_id`='%d' AND `char_id` = '%d'",
			charserv_table(char_table), p->base_level, p->job_level,
			p->base_exp, p->job_exp, p->zeny,
			p->max_hp, p->hp, p->max_sp, p->sp, p->status_point, p->skill_point,
			p->str, p->agi, p->vit, p->int_, p->dex, p->luk,
			p->option, p->party_id, p->guild_id, p->pet_id, p->hom_id, p->ele_id,
			p->weapon, p->shield, p->head_top, p->head_mid, p->head_bottom,
			mapindex_id2name(p->last_point.map), p->last_point.x, p->last_point.y,
			mapindex_id2name(p->save_point.map), p->save_point.x, p->save_point.y, p->rename,
			(unsigned long)p->delete_date, // FIXME: platform-dependent size
			p->robe, p->character_moves, p->font, p->uniqueitem_counter,
			p->hotkey_rowshift,
			p->account_id, p->char_id) )
		{
			Sql_ShowDebug(sql_handle);
			errors++;
		} else
			strcat(save_status, " status");
	}

	//Values that will seldom change (to speed up saving)
	if (
		(p->hair != cp->hair) || (p->hair_color != cp->hair_color) || (p->clothes_color != cp->clothes_color) ||
		(p->body != cp->body) || (p->class_ != cp->class_) ||
		(p->partner_id != cp->partner_id) || (p->father != cp->father) ||
		(p->mother != cp->mother) || (p->child != cp->child) ||
 		(p->karma != cp->karma) || (p->manner != cp->manner) ||
		(p->fame != cp->fame)
	)
	{
		if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `class`='%d',"
			"`hair`='%d', `hair_color`='%d', `clothes_color`='%d', `body`='%d',"
			"`partner_id`='%u', `father`='%u', `mother`='%u', `child`='%u',"
			"`karma`='%d',`manner`='%d', `fame`='%d'"
			" WHERE  `account_id`='%d' AND `char_id` = '%d'",
			charserv_table(char_table), p->class_,
			p->hair, p->hair_color, p->clothes_color, p->body,
			p->partner_id, p->father, p->mother, p->child,
			p->karma, p->manner, p->fame,
			p->account_id, p->char_id) )
		{
			Sql_ShowDebug(sql_handle);
			errors++;
		} else
			strcat(save_status, " status2");
	}

	/* Mercenary Owner */
	if( (p->mer_id != cp->mer_id) ||
		(p->arch_calls != cp->arch_calls) || (p->arch_faith != cp->arch_faith) ||
		(p->spear_calls != cp->spear_calls) || (p->spear_faith != cp->spear_faith) ||
		(p->sword_calls != cp->sword_calls) || (p->sword_faith != cp->sword_faith) )
	{
		if (mercenary_owner_tosql(char_id, p))
			strcat(save_status, " mercenary");
		else
			errors++;
	}

	//memo points
	if( memcmp(p->memo_point, cp->memo_point, sizeof(p->memo_point)) )
	{
		char esc_mapname[NAME_LENGTH*2+1];

		//`memo` (`memo_id`,`char_id`,`map`,`x`,`y`)
		if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `char_id`='%d'", charserv_table(memo_table), p->char_id) )
		{
			Sql_ShowDebug(sql_handle);
			errors++;
		}

		//insert here.
		StringBuf_Clear(&buf);
		StringBuf_Printf(&buf, "INSERT INTO `%s`(`char_id`,`map`,`x`,`y`) VALUES ", charserv_table(memo_table));
		for( i = 0, count = 0; i < MAX_MEMOPOINTS; ++i )
		{
			if( p->memo_point[i].map )
			{
				if( count )
					StringBuf_AppendStr(&buf, ",");
				Sql_EscapeString(sql_handle, esc_mapname, mapindex_id2name(p->memo_point[i].map));
				StringBuf_Printf(&buf, "('%d', '%s', '%d', '%d')", char_id, esc_mapname, p->memo_point[i].x, p->memo_point[i].y);
				++count;
			}
		}
		if( count )
		{
			if( SQL_ERROR == Sql_QueryStr(sql_handle, StringBuf_Value(&buf)) )
			{
				Sql_ShowDebug(sql_handle);
				errors++;
			}
		}
		strcat(save_status, " memo");
	}

	//skills
	if( memcmp(p->skill, cp->skill, sizeof(p->skill)) )
	{
		//`skill` (`char_id`, `id`, `lv`)
		if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `char_id`='%d'", charserv_table(skill_table), p->char_id) )
		{
			Sql_ShowDebug(sql_handle);
			errors++;
		}

		StringBuf_Clear(&buf);
		StringBuf_Printf(&buf, "INSERT INTO `%s`(`char_id`,`id`,`lv`,`flag`) VALUES ", charserv_table(skill_table));
		//insert here.
		for( i = 0, count = 0; i < MAX_SKILL; ++i ) {
			if( p->skill[i].id != 0 && p->skill[i].flag != SKILL_FLAG_TEMPORARY ) {

				if( p->skill[i].lv == 0 && ( p->skill[i].flag == SKILL_FLAG_PERM_GRANTED || p->skill[i].flag == SKILL_FLAG_PERMANENT ) )
					continue;
				if( p->skill[i].flag != SKILL_FLAG_PERMANENT && p->skill[i].flag != SKILL_FLAG_PERM_GRANTED && (p->skill[i].flag - SKILL_FLAG_REPLACED_LV_0) == 0 )
					continue;
				if( count )
					StringBuf_AppendStr(&buf, ",");
				StringBuf_Printf(&buf, "('%d','%d','%d','%d')", char_id, p->skill[i].id,
								 ( (p->skill[i].flag == SKILL_FLAG_PERMANENT || p->skill[i].flag == SKILL_FLAG_PERM_GRANTED) ? p->skill[i].lv : p->skill[i].flag - SKILL_FLAG_REPLACED_LV_0),
								 p->skill[i].flag == SKILL_FLAG_PERM_GRANTED ? p->skill[i].flag : 0);/* other flags do not need to be saved */
				++count;
			}
		}
		if( count )
		{
			if( SQL_ERROR == Sql_QueryStr(sql_handle, StringBuf_Value(&buf)) )
			{
				Sql_ShowDebug(sql_handle);
				errors++;
			}
		}

		strcat(save_status, " skills");
	}

	diff = 0;
	for(i = 0; i < MAX_FRIENDS; i++){
		if(p->friends[i].char_id != cp->friends[i].char_id ||
			p->friends[i].account_id != cp->friends[i].account_id){
			diff = 1;
			break;
		}
	}

	if(diff == 1)
	{	//Save friends
		if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `char_id`='%d'", charserv_table(friend_table), char_id) )
		{
			Sql_ShowDebug(sql_handle);
			errors++;
		}

		StringBuf_Clear(&buf);
		StringBuf_Printf(&buf, "INSERT INTO `%s` (`char_id`, `friend_account`, `friend_id`) VALUES ", charserv_table(friend_table));
		for( i = 0, count = 0; i < MAX_FRIENDS; ++i )
		{
			if( p->friends[i].char_id > 0 )
			{
				if( count )
					StringBuf_AppendStr(&buf, ",");
				StringBuf_Printf(&buf, "('%d','%d','%d')", char_id, p->friends[i].account_id, p->friends[i].char_id);
				count++;
			}
		}
		if( count )
		{
			if( SQL_ERROR == Sql_QueryStr(sql_handle, StringBuf_Value(&buf)) )
			{
				Sql_ShowDebug(sql_handle);
				errors++;
			}
		}
		strcat(save_status, " friends");
	}

#ifdef HOTKEY_SAVING
	// hotkeys
	StringBuf_Clear(&buf);
	StringBuf_Printf(&buf, "REPLACE INTO `%s` (`char_id`, `hotkey`, `type`, `itemskill_id`, `skill_lvl`) VALUES ", charserv_table(hotkey_table));
	diff = 0;
	for(i = 0; i < ARRAYLENGTH(p->hotkeys); i++){
		if(memcmp(&p->hotkeys[i], &cp->hotkeys[i], sizeof(struct hotkey)))
		{
			if( diff )
				StringBuf_AppendStr(&buf, ",");// not the first hotkey
			StringBuf_Printf(&buf, "('%d','%u','%u','%u','%u')", char_id, (unsigned int)i, (unsigned int)p->hotkeys[i].type, p->hotkeys[i].id , (unsigned int)p->hotkeys[i].lv);
			diff = 1;
		}
	}
	if(diff) {
		if( SQL_ERROR == Sql_QueryStr(sql_handle, StringBuf_Value(&buf)) )
		{
			Sql_ShowDebug(sql_handle);
			errors++;
		} else
			strcat(save_status, " hotkeys");
	}
#endif
	StringBuf_Destroy(&buf);
	if (save_status[0]!='\0' && charserv_config.save_log)
		ShowInfo("Saved char %d - %s:%s.\n", char_id, p->name, save_status);
	if (!errors)
		memcpy(cp, p, sizeof(struct mmo_charstatus));
	return 0;
}

/// Saves an array of 'item' entries into the specified table.
int char_memitemdata_to_sql(const struct item items[], int max, int id, int tableswitch){
	StringBuf buf;
	SqlStmt* stmt;
	int i;
	int j;
	const char* tablename;
	const char* selectoption;
	struct item item; // temp storage variable
	bool* flag; // bit array for inventory matching
	bool found;
	int errors = 0;

	switch (tableswitch) {
	case TABLE_INVENTORY:     tablename = charserv_table(inventory_table);     selectoption = "char_id";    break;
	case TABLE_CART:          tablename = charserv_table(cart_table);          selectoption = "char_id";    break;
	case TABLE_STORAGE:       tablename = charserv_table(storage_table);       selectoption = "account_id"; break;
	case TABLE_GUILD_STORAGE: tablename = charserv_table(guild_storage_table); selectoption = "guild_id";   break;
	default:
		ShowError("Invalid table name!\n");
		return 1;
	}


	// The following code compares inventory with current database values
	// and performs modification/deletion/insertion only on relevant rows.
	// This approach is more complicated than a trivial delete&insert, but
	// it significantly reduces cpu load on the database server.

	StringBuf_Init(&buf);
	StringBuf_AppendStr(&buf, "SELECT `id`, `nameid`, `amount`, `equip`, `identify`, `refine`, `attribute`, `expire_time`, `bound`, `unique_id`");
	for( j = 0; j < MAX_SLOTS; ++j )
		StringBuf_Printf(&buf, ", `card%d`", j);
	StringBuf_Printf(&buf, " FROM `%s` WHERE `%s`='%d'", tablename, selectoption, id);

	stmt = SqlStmt_Malloc(sql_handle);
	if( SQL_ERROR == SqlStmt_PrepareStr(stmt, StringBuf_Value(&buf))
	||  SQL_ERROR == SqlStmt_Execute(stmt) )
	{
		SqlStmt_ShowDebug(stmt);
		SqlStmt_Free(stmt);
		StringBuf_Destroy(&buf);
		return 1;
	}

	SqlStmt_BindColumn(stmt, 0, SQLDT_INT,       &item.id,          0, NULL, NULL);
	SqlStmt_BindColumn(stmt, 1, SQLDT_USHORT,    &item.nameid,      0, NULL, NULL);
	SqlStmt_BindColumn(stmt, 2, SQLDT_SHORT,     &item.amount,      0, NULL, NULL);
	SqlStmt_BindColumn(stmt, 3, SQLDT_UINT,      &item.equip,       0, NULL, NULL);
	SqlStmt_BindColumn(stmt, 4, SQLDT_CHAR,      &item.identify,    0, NULL, NULL);
	SqlStmt_BindColumn(stmt, 5, SQLDT_CHAR,      &item.refine,      0, NULL, NULL);
	SqlStmt_BindColumn(stmt, 6, SQLDT_CHAR,      &item.attribute,   0, NULL, NULL);
	SqlStmt_BindColumn(stmt, 7, SQLDT_UINT,      &item.expire_time, 0, NULL, NULL);
	SqlStmt_BindColumn(stmt, 8, SQLDT_UINT,      &item.bound,       0, NULL, NULL);
	SqlStmt_BindColumn(stmt, 9, SQLDT_UINT64,    &item.unique_id,   0, NULL, NULL);
	for( j = 0; j < MAX_SLOTS; ++j )
		SqlStmt_BindColumn(stmt, 10+j, SQLDT_USHORT, &item.card[j], 0, NULL, NULL);

	// bit array indicating which inventory items have already been matched
	flag = (bool*) aCalloc(max, sizeof(bool));

	while( SQL_SUCCESS == SqlStmt_NextRow(stmt) )
	{
		found = false;
		// search for the presence of the item in the char's inventory
		for( i = 0; i < max; ++i )
		{
			// skip empty and already matched entries
			if( items[i].nameid == 0 || flag[i] )
				continue;

			if( items[i].nameid == item.nameid
			&&  items[i].card[0] == item.card[0]
			&&  items[i].card[2] == item.card[2]
			&&  items[i].card[3] == item.card[3]
			) {	//They are the same item.
				ARR_FIND( 0, MAX_SLOTS, j, items[i].card[j] != item.card[j] );
				if( j == MAX_SLOTS &&
				    items[i].amount == item.amount &&
				    items[i].equip == item.equip &&
				    items[i].identify == item.identify &&
				    items[i].refine == item.refine &&
				    items[i].attribute == item.attribute &&
				    items[i].expire_time == item.expire_time &&
				    items[i].bound == item.bound &&
					items[i].unique_id == item.unique_id )
				;	//Do nothing.
				else
				{
					// update all fields.
					StringBuf_Clear(&buf);
					StringBuf_Printf(&buf, "UPDATE `%s` SET `amount`='%d', `equip`='%d', `identify`='%d', `refine`='%d',`attribute`='%d', `expire_time`='%u', `bound`='%d', `unique_id`='%"PRIu64"'",
						tablename, items[i].amount, items[i].equip, items[i].identify, items[i].refine, items[i].attribute, items[i].expire_time, items[i].bound, items[i].unique_id);
					for( j = 0; j < MAX_SLOTS; ++j )
						StringBuf_Printf(&buf, ", `card%d`=%hu", j, items[i].card[j]);
					StringBuf_Printf(&buf, " WHERE `id`='%d' LIMIT 1", item.id);

					if( SQL_ERROR == Sql_QueryStr(sql_handle, StringBuf_Value(&buf)) )
					{
						Sql_ShowDebug(sql_handle);
						errors++;
					}
				}

				found = flag[i] = true; //Item dealt with,
				break; //skip to next item in the db.
			}
		}
		if( !found )
		{// Item not present in inventory, remove it.
			if( SQL_ERROR == Sql_Query(sql_handle, "DELETE from `%s` where `id`='%d' LIMIT 1", tablename, item.id) )
			{
				Sql_ShowDebug(sql_handle);
				errors++;
			}
		}
	}
	SqlStmt_Free(stmt);

	StringBuf_Clear(&buf);
	StringBuf_Printf(&buf, "INSERT INTO `%s`(`%s`, `nameid`, `amount`, `equip`, `identify`, `refine`, `attribute`, `expire_time`, `bound`, `unique_id`", tablename, selectoption);
	for( j = 0; j < MAX_SLOTS; ++j )
		StringBuf_Printf(&buf, ", `card%d`", j);
	StringBuf_AppendStr(&buf, ") VALUES ");

	found = false;
	// insert non-matched items into the db as new items
	for( i = 0; i < max; ++i )
	{
		// skip empty and already matched entries
		if( items[i].nameid == 0 || flag[i] )
			continue;

		if( found )
			StringBuf_AppendStr(&buf, ",");
		else
			found = true;

		StringBuf_Printf(&buf, "('%d', '%hu', '%d', '%d', '%d', '%d', '%d', '%u', '%d', '%"PRIu64"'",
			id, items[i].nameid, items[i].amount, items[i].equip, items[i].identify, items[i].refine, items[i].attribute, items[i].expire_time, items[i].bound, items[i].unique_id);
		for( j = 0; j < MAX_SLOTS; ++j )
			StringBuf_Printf(&buf, ", '%hu'", items[i].card[j]);
		StringBuf_AppendStr(&buf, ")");
	}

	if( found && SQL_ERROR == Sql_QueryStr(sql_handle, StringBuf_Value(&buf)) )
	{
		Sql_ShowDebug(sql_handle);
		errors++;
	}

	StringBuf_Destroy(&buf);
	aFree(flag);

	return errors;
}
/* pretty much a copy of memitemdata_to_sql except it handles inventory_db exclusively,
 * - this is required because inventory db is the only one with the 'favorite' column. */
int char_inventory_to_sql(const struct item items[], int max, int id) {
	StringBuf buf;
	SqlStmt* stmt;
	int i;
	int j;
	struct item item; // temp storage variable
	bool* flag; // bit array for inventory matching
	bool found;
	int errors = 0;


	// The following code compares inventory with current database values
	// and performs modification/deletion/insertion only on relevant rows.
	// This approach is more complicated than a trivial delete&insert, but
	// it significantly reduces cpu load on the database server.

	StringBuf_Init(&buf);
	StringBuf_AppendStr(&buf, "SELECT `id`, `nameid`, `amount`, `equip`, `identify`, `refine`, `attribute`, `expire_time`, `favorite`, `bound`, `unique_id`");
	for( j = 0; j < MAX_SLOTS; ++j )
		StringBuf_Printf(&buf, ", `card%d`", j);
	StringBuf_Printf(&buf, " FROM `%s` WHERE `char_id`='%d'", charserv_table(inventory_table), id);

	stmt = SqlStmt_Malloc(sql_handle);
	if( SQL_ERROR == SqlStmt_PrepareStr(stmt, StringBuf_Value(&buf))
	   ||  SQL_ERROR == SqlStmt_Execute(stmt) )
	{
		SqlStmt_ShowDebug(stmt);
		SqlStmt_Free(stmt);
		StringBuf_Destroy(&buf);
		return 1;
	}

	SqlStmt_BindColumn(stmt, 0, SQLDT_INT,       &item.id,          0, NULL, NULL);
	SqlStmt_BindColumn(stmt, 1, SQLDT_USHORT,    &item.nameid,      0, NULL, NULL);
	SqlStmt_BindColumn(stmt, 2, SQLDT_SHORT,     &item.amount,      0, NULL, NULL);
	SqlStmt_BindColumn(stmt, 3, SQLDT_UINT,      &item.equip,       0, NULL, NULL);
	SqlStmt_BindColumn(stmt, 4, SQLDT_CHAR,      &item.identify,    0, NULL, NULL);
	SqlStmt_BindColumn(stmt, 5, SQLDT_CHAR,      &item.refine,      0, NULL, NULL);
	SqlStmt_BindColumn(stmt, 6, SQLDT_CHAR,      &item.attribute,   0, NULL, NULL);
	SqlStmt_BindColumn(stmt, 7, SQLDT_UINT,      &item.expire_time, 0, NULL, NULL);
	SqlStmt_BindColumn(stmt, 8, SQLDT_CHAR,      &item.favorite,    0, NULL, NULL);
	SqlStmt_BindColumn(stmt, 9, SQLDT_CHAR,      &item.bound,       0, NULL, NULL);
	SqlStmt_BindColumn(stmt, 10,SQLDT_UINT64,    &item.unique_id,   0, NULL, NULL);
	for( j = 0; j < MAX_SLOTS; ++j )
		SqlStmt_BindColumn(stmt, 11+j, SQLDT_USHORT, &item.card[j], 0, NULL, NULL);

	// bit array indicating which inventory items have already been matched
	flag = (bool*) aCalloc(max, sizeof(bool));

	while( SQL_SUCCESS == SqlStmt_NextRow(stmt) ) {
		found = false;
		// search for the presence of the item in the char's inventory
		for( i = 0; i < max; ++i ) {
			// skip empty and already matched entries
			if( items[i].nameid == 0 || flag[i] )
				continue;

			if( items[i].nameid == item.nameid
			   &&  items[i].card[0] == item.card[0]
			   &&  items[i].card[2] == item.card[2]
			   &&  items[i].card[3] == item.card[3]
			   ) {	//They are the same item.
				ARR_FIND( 0, MAX_SLOTS, j, items[i].card[j] != item.card[j] );
				if( j == MAX_SLOTS &&
					items[i].amount == item.amount &&
					items[i].equip == item.equip &&
					items[i].identify == item.identify &&
					items[i].refine == item.refine &&
					items[i].attribute == item.attribute &&
					items[i].expire_time == item.expire_time &&
					items[i].favorite == item.favorite &&
					items[i].bound == item.bound &&
					items[i].unique_id == item.unique_id )
					;	//Do nothing.
				else {
					// update all fields.
					StringBuf_Clear(&buf);
					StringBuf_Printf(&buf, "UPDATE `%s` SET `amount`='%d', `equip`='%d', `identify`='%d', `refine`='%d',`attribute`='%d', `expire_time`='%u', `favorite`='%d', `bound`='%d', `unique_id`='%"PRIu64"'",
					    charserv_table(inventory_table), items[i].amount, items[i].equip, items[i].identify, items[i].refine, items[i].attribute, items[i].expire_time, items[i].favorite, items[i].bound, items[i].unique_id);
					for( j = 0; j < MAX_SLOTS; ++j )
						StringBuf_Printf(&buf, ", `card%d`=%hu", j, items[i].card[j]);
					StringBuf_Printf(&buf, " WHERE `id`='%d' LIMIT 1", item.id);

					if( SQL_ERROR == Sql_QueryStr(sql_handle, StringBuf_Value(&buf)) ) {
						Sql_ShowDebug(sql_handle);
						errors++;
					}
				}

				found = flag[i] = true; //Item dealt with,
				break; //skip to next item in the db.
			}
		}
		if( !found ) {// Item not present in inventory, remove it.
			if( SQL_ERROR == Sql_Query(sql_handle, "DELETE from `%s` where `id`='%d' LIMIT 1", charserv_table(inventory_table), item.id) ) {
				Sql_ShowDebug(sql_handle);
				errors++;
			}
		}
	}
	SqlStmt_Free(stmt);

	StringBuf_Clear(&buf);
	StringBuf_Printf(&buf, "INSERT INTO `%s` (`char_id`, `nameid`, `amount`, `equip`, `identify`, `refine`, `attribute`, `expire_time`, `favorite`, `bound`, `unique_id`", charserv_table(inventory_table));
	for( j = 0; j < MAX_SLOTS; ++j )
		StringBuf_Printf(&buf, ", `card%d`", j);
	StringBuf_AppendStr(&buf, ") VALUES ");

	found = false;
	// insert non-matched items into the db as new items
	for( i = 0; i < max; ++i ) {
		// skip empty and already matched entries
		if( items[i].nameid == 0 || flag[i] )
			continue;

		if( found )
			StringBuf_AppendStr(&buf, ",");
		else
			found = true;

		StringBuf_Printf(&buf, "('%d', '%hu', '%d', '%d', '%d', '%d', '%d', '%u', '%d', '%d', '%"PRIu64"'",
						 id, items[i].nameid, items[i].amount, items[i].equip, items[i].identify, items[i].refine, items[i].attribute, items[i].expire_time, items[i].favorite, items[i].bound, items[i].unique_id);
		for( j = 0; j < MAX_SLOTS; ++j )
			StringBuf_Printf(&buf, ", '%hu'", items[i].card[j]);
		StringBuf_AppendStr(&buf, ")");
	}

	if( found && SQL_ERROR == Sql_QueryStr(sql_handle, StringBuf_Value(&buf)) ) {
		Sql_ShowDebug(sql_handle);
		errors++;
	}

	StringBuf_Destroy(&buf);
	aFree(flag);

	return errors;
}

/**
 * Returns the correct gender ID for the given character and enum value.
 *
 * If the per-character sex is defined but not supported by the current packetver, the database entries are corrected.
 *
 * @param sd Character data, if available.
 * @param p  Character status.
 * @param sex Character sex (database enum)
 *
 * @retval SEX_MALE if the per-character sex is male
 * @retval SEX_FEMALE if the per-character sex is female
 * @retval 99 if the per-character sex is not defined or the current PACKETVER doesn't support it.
 */
int char_mmo_gender(const struct char_session_data *sd, const struct mmo_charstatus *p, char sex)
{
#if PACKETVER >= 20141016
	(void)sd; (void)p; // Unused
	switch (sex) {
		case 'M':
			return SEX_MALE;
		case 'F':
			return SEX_FEMALE;
		case 'U':
		default:
			return 99;
	}
#else
	if (sex == 'M' || sex == 'F') {
		if (!sd) {
			// sd is not available, there isn't much we can do. Just return and print a warning.
			ShowWarning("Character '%s' (CID: %d, AID: %d) has sex '%c', but PACKETVER does not support per-character sex. Defaulting to 'U'.\n",
					p->name, p->char_id, p->account_id, sex);
			return 99;
		}
		if ((sex == 'M' && sd->sex == SEX_FEMALE)
		 || (sex == 'F' && sd->sex == SEX_MALE)) {
			ShowWarning("Changing sex of character '%s' (CID: %d, AID: %d) to 'U' due to incompatible PACKETVER.\n", p->name, p->char_id, p->account_id);
			chlogif_parse_ackchangecharsex(p->char_id, sd->sex);
		} else {
			ShowInfo("Resetting sex of character '%s' (CID: %d, AID: %d) to 'U' due to incompatible PACKETVER.\n", p->name, p->char_id, p->account_id);
		}
		if (SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `sex` = 'U' WHERE `char_id` = '%d'", charserv_table(char_table), p->char_id)) {
			Sql_ShowDebug(sql_handle);
		}
	}
	return 99;
#endif
}

int char_mmo_char_tobuf(uint8* buf, struct mmo_charstatus* p);

//=====================================================================================================
// Loads the basic character rooster for the given account. Returns total buffer used.
int char_mmo_chars_fromsql(struct char_session_data* sd, uint8* buf) {
	SqlStmt* stmt;
	struct mmo_charstatus p;
	int j = 0, i;
	char last_map[MAP_NAME_LENGTH_EXT];
	char sex[2];

	stmt = SqlStmt_Malloc(sql_handle);
	if( stmt == NULL ) {
		SqlStmt_ShowDebug(stmt);
		return 0;
	}
	memset(&p, 0, sizeof(p));

	for( i = 0; i < MAX_CHARS; i++ ) {
		sd->found_char[i] = -1;
		sd->unban_time[i] = 0;
	}

	// read char data
	if( SQL_ERROR == SqlStmt_Prepare(stmt, "SELECT "
		"`char_id`,`char_num`,`name`,`class`,`base_level`,`job_level`,`base_exp`,`job_exp`,`zeny`,"
		"`str`,`agi`,`vit`,`int`,`dex`,`luk`,`max_hp`,`hp`,`max_sp`,`sp`,"
		"`status_point`,`skill_point`,`option`,`karma`,`manner`,`hair`,`hair_color`,"
		"`clothes_color`,`body`,`weapon`,`shield`,`head_top`,`head_mid`,`head_bottom`,`last_map`,`rename`,`delete_date`,"
		"`robe`,`moves`,`unban_time`,`font`,`uniqueitem_counter`,`sex`,`hotkey_rowshift`"
		" FROM `%s` WHERE `account_id`='%d' AND `char_num` < '%d'", charserv_table(char_table), sd->account_id, MAX_CHARS)
	||	SQL_ERROR == SqlStmt_Execute(stmt)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 0,  SQLDT_INT,    &p.char_id, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 1,  SQLDT_UCHAR,  &p.slot, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 2,  SQLDT_STRING, &p.name, sizeof(p.name), NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 3,  SQLDT_SHORT,  &p.class_, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 4,  SQLDT_UINT,   &p.base_level, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 5,  SQLDT_UINT,   &p.job_level, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 6,  SQLDT_UINT,   &p.base_exp, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 7,  SQLDT_UINT,   &p.job_exp, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 8,  SQLDT_INT,    &p.zeny, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 9,  SQLDT_SHORT,  &p.str, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 10, SQLDT_SHORT,  &p.agi, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 11, SQLDT_SHORT,  &p.vit, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 12, SQLDT_SHORT,  &p.int_, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 13, SQLDT_SHORT,  &p.dex, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 14, SQLDT_SHORT,  &p.luk, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 15, SQLDT_INT,    &p.max_hp, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 16, SQLDT_INT,    &p.hp, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 17, SQLDT_INT,    &p.max_sp, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 18, SQLDT_INT,    &p.sp, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 19, SQLDT_UINT,   &p.status_point, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 20, SQLDT_UINT,   &p.skill_point, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 21, SQLDT_UINT,   &p.option, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 22, SQLDT_UCHAR,  &p.karma, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 23, SQLDT_SHORT,  &p.manner, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 24, SQLDT_SHORT,  &p.hair, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 25, SQLDT_SHORT,  &p.hair_color, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 26, SQLDT_SHORT,  &p.clothes_color, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 27, SQLDT_SHORT,  &p.body, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 28, SQLDT_SHORT,  &p.weapon, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 29, SQLDT_SHORT,  &p.shield, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 30, SQLDT_SHORT,  &p.head_top, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 31, SQLDT_SHORT,  &p.head_mid, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 32, SQLDT_SHORT,  &p.head_bottom, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 33, SQLDT_STRING, &last_map, sizeof(last_map), NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 34, SQLDT_SHORT,	&p.rename, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 35, SQLDT_UINT32, &p.delete_date, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 36, SQLDT_SHORT,  &p.robe, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 37, SQLDT_UINT,   &p.character_moves, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 38, SQLDT_LONG,   &p.unban_time, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 39, SQLDT_UCHAR,  &p.font, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 40, SQLDT_UINT,   &p.uniqueitem_counter, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 41, SQLDT_ENUM,   &sex, sizeof(sex), NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 42, SQLDT_UCHAR,   &p.hotkey_rowshift, 0, NULL, NULL)
	)
	{
		SqlStmt_ShowDebug(stmt);
		SqlStmt_Free(stmt);
		return 0;
	}

	for( i = 0; i < MAX_CHARS && SQL_SUCCESS == SqlStmt_NextRow(stmt); i++ )
	{
		p.last_point.map = mapindex_name2id(last_map);
		sd->found_char[p.slot] = p.char_id;
		sd->unban_time[p.slot] = p.unban_time;
		p.sex = char_mmo_gender(sd, &p, sex[0]);
		j += char_mmo_char_tobuf(WBUFP(buf, j), &p);

		// Addon System
		// store the required info into the session
		sd->char_moves[p.slot] = p.character_moves;
	}

	memset(sd->new_name,0,sizeof(sd->new_name));

	SqlStmt_Free(stmt);
	return j;
}

//=====================================================================================================
int char_mmo_char_fromsql(uint32 char_id, struct mmo_charstatus* p, bool load_everything) {
	int i,j;
	struct mmo_charstatus* cp;
	StringBuf buf;
	SqlStmt* stmt;
	char last_map[MAP_NAME_LENGTH_EXT];
	char save_map[MAP_NAME_LENGTH_EXT];
	char point_map[MAP_NAME_LENGTH_EXT];
	struct point tmp_point;
	struct item tmp_item;
	struct s_skill tmp_skill;
	uint16 skill_count = 0;
	struct s_friend tmp_friend;
#ifdef HOTKEY_SAVING
	struct hotkey tmp_hotkey;
	int hotkey_num;
#endif
	StringBuf msg_buf;
	char sex[2];

	memset(p, 0, sizeof(struct mmo_charstatus));

	if (charserv_config.save_log) ShowInfo("Char load request (%d)\n", char_id);

	stmt = SqlStmt_Malloc(sql_handle);
	if( stmt == NULL )
	{
		SqlStmt_ShowDebug(stmt);
		return 0;
	}

	// read char data
	if( SQL_ERROR == SqlStmt_Prepare(stmt, "SELECT "
		"`char_id`,`account_id`,`char_num`,`name`,`class`,`base_level`,`job_level`,`base_exp`,`job_exp`,`zeny`,"
		"`str`,`agi`,`vit`,`int`,`dex`,`luk`,`max_hp`,`hp`,`max_sp`,`sp`,"
		"`status_point`,`skill_point`,`option`,`karma`,`manner`,`party_id`,`guild_id`,`pet_id`,`homun_id`,`elemental_id`,`hair`,"
		"`hair_color`,`clothes_color`,`body`,`weapon`,`shield`,`head_top`,`head_mid`,`head_bottom`,`last_map`,`last_x`,`last_y`,"
		"`save_map`,`save_x`,`save_y`,`partner_id`,`father`,`mother`,`child`,`fame`,`rename`,`delete_date`,`robe`, `moves`,"
		"`unban_time`,`font`,`uniqueitem_counter`,`sex`,`hotkey_rowshift`"
		" FROM `%s` WHERE `char_id`=? LIMIT 1", charserv_table(char_table))
	||	SQL_ERROR == SqlStmt_BindParam(stmt, 0, SQLDT_INT, &char_id, 0)
	||	SQL_ERROR == SqlStmt_Execute(stmt)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 0,  SQLDT_INT,    &p->char_id, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 1,  SQLDT_INT,    &p->account_id, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 2,  SQLDT_UCHAR,  &p->slot, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 3,  SQLDT_STRING, &p->name, sizeof(p->name), NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 4,  SQLDT_SHORT,  &p->class_, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 5,  SQLDT_UINT,   &p->base_level, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 6,  SQLDT_UINT,   &p->job_level, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 7,  SQLDT_UINT,   &p->base_exp, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 8,  SQLDT_UINT,   &p->job_exp, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 9,  SQLDT_INT,    &p->zeny, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 10, SQLDT_SHORT,  &p->str, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 11, SQLDT_SHORT,  &p->agi, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 12, SQLDT_SHORT,  &p->vit, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 13, SQLDT_SHORT,  &p->int_, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 14, SQLDT_SHORT,  &p->dex, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 15, SQLDT_SHORT,  &p->luk, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 16, SQLDT_INT,    &p->max_hp, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 17, SQLDT_INT,    &p->hp, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 18, SQLDT_INT,    &p->max_sp, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 19, SQLDT_INT,    &p->sp, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 20, SQLDT_UINT,   &p->status_point, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 21, SQLDT_UINT,   &p->skill_point, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 22, SQLDT_UINT,   &p->option, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 23, SQLDT_UCHAR,  &p->karma, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 24, SQLDT_SHORT,  &p->manner, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 25, SQLDT_INT,    &p->party_id, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 26, SQLDT_INT,    &p->guild_id, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 27, SQLDT_INT,    &p->pet_id, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 28, SQLDT_INT,    &p->hom_id, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 29, SQLDT_INT,    &p->ele_id, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 30, SQLDT_SHORT,  &p->hair, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 31, SQLDT_SHORT,  &p->hair_color, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 32, SQLDT_SHORT,  &p->clothes_color, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 33, SQLDT_SHORT,  &p->body, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 34, SQLDT_SHORT,  &p->weapon, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 35, SQLDT_SHORT,  &p->shield, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 36, SQLDT_SHORT,  &p->head_top, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 37, SQLDT_SHORT,  &p->head_mid, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 38, SQLDT_SHORT,  &p->head_bottom, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 39, SQLDT_STRING, &last_map, sizeof(last_map), NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 40, SQLDT_SHORT,  &p->last_point.x, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 41, SQLDT_SHORT,  &p->last_point.y, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 42, SQLDT_STRING, &save_map, sizeof(save_map), NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 43, SQLDT_SHORT,  &p->save_point.x, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 44, SQLDT_SHORT,  &p->save_point.y, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 45, SQLDT_UINT32,    &p->partner_id, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 46, SQLDT_UINT32,    &p->father, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 47, SQLDT_UINT32,    &p->mother, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 48, SQLDT_UINT32,    &p->child, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 49, SQLDT_INT,    &p->fame, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 50, SQLDT_SHORT,  &p->rename, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 51, SQLDT_UINT32, &p->delete_date, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 52, SQLDT_SHORT,  &p->robe, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 53, SQLDT_UINT32, &p->character_moves, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 54, SQLDT_LONG,   &p->unban_time, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 55, SQLDT_UCHAR,  &p->font, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 56, SQLDT_UINT,   &p->uniqueitem_counter, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 57, SQLDT_ENUM,   &sex, sizeof(sex), NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 58, SQLDT_UCHAR,  &p->hotkey_rowshift, 0, NULL, NULL)
	)
	{
		SqlStmt_ShowDebug(stmt);
		SqlStmt_Free(stmt);
		return 0;
	}
	if( SQL_ERROR == SqlStmt_NextRow(stmt) )
	{
		ShowError("Requested non-existant character id: %d!\n", char_id);
		SqlStmt_Free(stmt);
		return 0;
	}
	p->sex = char_mmo_gender(NULL, p, sex[0]);
	p->last_point.map = mapindex_name2id(last_map);
	p->save_point.map = mapindex_name2id(save_map);

	if( p->last_point.map == 0 ) {
		p->last_point.map = mapindex_name2id(charserv_config.default_map);
		p->last_point.x = charserv_config.default_map_x;
		p->last_point.y = charserv_config.default_map_y;
	}

	if( p->save_point.map == 0 ) {
		p->save_point.map = mapindex_name2id(charserv_config.default_map);
		p->save_point.x = charserv_config.default_map_x;
		p->save_point.y = charserv_config.default_map_y;
	}

	StringBuf_Init(&msg_buf);
	StringBuf_AppendStr(&msg_buf, " status");

	if (!load_everything) // For quick selection of data when displaying the char menu
	{
		SqlStmt_Free(stmt);
		StringBuf_Destroy(&msg_buf);
		return 1;
	}

	//read memo data
	//`memo` (`memo_id`,`char_id`,`map`,`x`,`y`)
	if( SQL_ERROR == SqlStmt_Prepare(stmt, "SELECT `map`,`x`,`y` FROM `%s` WHERE `char_id`=? ORDER by `memo_id` LIMIT %d", charserv_table(memo_table), MAX_MEMOPOINTS)
	||	SQL_ERROR == SqlStmt_BindParam(stmt, 0, SQLDT_INT, &char_id, 0)
	||	SQL_ERROR == SqlStmt_Execute(stmt)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 0, SQLDT_STRING, &point_map, sizeof(point_map), NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 1, SQLDT_SHORT,  &tmp_point.x, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 2, SQLDT_SHORT,  &tmp_point.y, 0, NULL, NULL) )
		SqlStmt_ShowDebug(stmt);

	for( i = 0; i < MAX_MEMOPOINTS && SQL_SUCCESS == SqlStmt_NextRow(stmt); ++i )
	{
		tmp_point.map = mapindex_name2id(point_map);
		memcpy(&p->memo_point[i], &tmp_point, sizeof(tmp_point));
	}
	StringBuf_AppendStr(&msg_buf, " memo");

	//read inventory
	//`inventory` (`id`,`char_id`, `nameid`, `amount`, `equip`, `identify`, `refine`, `attribute`, `card0`, `card1`, `card2`, `card3`, `expire_time`, `favorite`, `unique_id`)
	StringBuf_Init(&buf);
	StringBuf_AppendStr(&buf, "SELECT `id`, `nameid`, `amount`, `equip`, `identify`, `refine`, `attribute`, `expire_time`, `favorite`, `bound`, `unique_id`");
	for( i = 0; i < MAX_SLOTS; ++i )
		StringBuf_Printf(&buf, ", `card%d`", i);
	StringBuf_Printf(&buf, " FROM `%s` WHERE `char_id`=? LIMIT %d", charserv_table(inventory_table), MAX_INVENTORY);

	if( SQL_ERROR == SqlStmt_PrepareStr(stmt, StringBuf_Value(&buf))
	||	SQL_ERROR == SqlStmt_BindParam(stmt, 0, SQLDT_INT, &char_id, 0)
	||	SQL_ERROR == SqlStmt_Execute(stmt)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 0, SQLDT_INT,       &tmp_item.id, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 1, SQLDT_USHORT,    &tmp_item.nameid, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 2, SQLDT_SHORT,     &tmp_item.amount, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 3, SQLDT_UINT,      &tmp_item.equip, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 4, SQLDT_CHAR,      &tmp_item.identify, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 5, SQLDT_CHAR,      &tmp_item.refine, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 6, SQLDT_CHAR,      &tmp_item.attribute, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 7, SQLDT_UINT,      &tmp_item.expire_time, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 8, SQLDT_CHAR,      &tmp_item.favorite, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 9, SQLDT_CHAR,      &tmp_item.bound, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt,10, SQLDT_UINT64,    &tmp_item.unique_id, 0, NULL, NULL) )
		SqlStmt_ShowDebug(stmt);
	for( i = 0; i < MAX_SLOTS; ++i )
		if( SQL_ERROR == SqlStmt_BindColumn(stmt, 11+i, SQLDT_USHORT, &tmp_item.card[i], 0, NULL, NULL) )
			SqlStmt_ShowDebug(stmt);

	for( i = 0; i < MAX_INVENTORY && SQL_SUCCESS == SqlStmt_NextRow(stmt); ++i )
		memcpy(&p->inventory[i], &tmp_item, sizeof(tmp_item));

	StringBuf_AppendStr(&msg_buf, " inventory");

	//read cart
	//`cart_inventory` (`id`,`char_id`, `nameid`, `amount`, `equip`, `identify`, `refine`, `attribute`, `card0`, `card1`, `card2`, `card3`, expire_time`, `unique_id`)
	StringBuf_Clear(&buf);
	StringBuf_AppendStr(&buf, "SELECT `id`, `nameid`, `amount`, `equip`, `identify`, `refine`, `attribute`, `expire_time`, `bound`, `unique_id`");
	for( j = 0; j < MAX_SLOTS; ++j )
		StringBuf_Printf(&buf, ", `card%d`", j);
	StringBuf_Printf(&buf, " FROM `%s` WHERE `char_id`=? LIMIT %d", charserv_table(cart_table), MAX_CART);

	if( SQL_ERROR == SqlStmt_PrepareStr(stmt, StringBuf_Value(&buf))
	||	SQL_ERROR == SqlStmt_BindParam(stmt, 0, SQLDT_INT, &char_id, 0)
	||	SQL_ERROR == SqlStmt_Execute(stmt)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 0, SQLDT_INT,         &tmp_item.id, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 1, SQLDT_USHORT,      &tmp_item.nameid, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 2, SQLDT_SHORT,       &tmp_item.amount, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 3, SQLDT_UINT,        &tmp_item.equip, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 4, SQLDT_CHAR,        &tmp_item.identify, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 5, SQLDT_CHAR,        &tmp_item.refine, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 6, SQLDT_CHAR,        &tmp_item.attribute, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 7, SQLDT_UINT,        &tmp_item.expire_time, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 8, SQLDT_CHAR,        &tmp_item.bound, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 9, SQLDT_UINT64,      &tmp_item.unique_id, 0, NULL, NULL) )
		SqlStmt_ShowDebug(stmt);
	for( i = 0; i < MAX_SLOTS; ++i )
		if( SQL_ERROR == SqlStmt_BindColumn(stmt, 10+i, SQLDT_USHORT, &tmp_item.card[i], 0, NULL, NULL) )
			SqlStmt_ShowDebug(stmt);

	for( i = 0; i < MAX_CART && SQL_SUCCESS == SqlStmt_NextRow(stmt); ++i )
		memcpy(&p->cart[i], &tmp_item, sizeof(tmp_item));
	StringBuf_AppendStr(&msg_buf, " cart");

	//read storage
	storage_fromsql(p->account_id, &p->storage);
	StringBuf_AppendStr(&msg_buf, " storage");

	//read skill
	//`skill` (`char_id`, `id`, `lv`)
	if( SQL_ERROR == SqlStmt_Prepare(stmt, "SELECT `id`, `lv`,`flag` FROM `%s` WHERE `char_id`=? LIMIT %d", charserv_table(skill_table), MAX_SKILL)
	||	SQL_ERROR == SqlStmt_BindParam(stmt, 0, SQLDT_INT, &char_id, 0)
	||	SQL_ERROR == SqlStmt_Execute(stmt)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 0, SQLDT_UINT16, &tmp_skill.id  , 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 1, SQLDT_UINT8 , &tmp_skill.lv  , 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 2, SQLDT_UINT8 , &tmp_skill.flag, 0, NULL, NULL) )
		SqlStmt_ShowDebug(stmt);

	if( tmp_skill.flag != SKILL_FLAG_PERM_GRANTED )
		tmp_skill.flag = SKILL_FLAG_PERMANENT;

	for( i = 0; skill_count < MAX_SKILL && SQL_SUCCESS == SqlStmt_NextRow(stmt); i++ ) {
		if( tmp_skill.id > 0 && tmp_skill.id < MAX_SKILL_ID ) {
			memcpy(&p->skill[i], &tmp_skill, sizeof(tmp_skill));
			skill_count++;
		}
		else
			ShowWarning("mmo_char_fromsql: ignoring invalid skill (id=%u,lv=%u) of character %s (AID=%d,CID=%d)\n", tmp_skill.id, tmp_skill.lv, p->name, p->account_id, p->char_id);
	}
	StringBuf_Printf(&msg_buf, " %d skills", skill_count);

	//read friends
	//`friends` (`char_id`, `friend_account`, `friend_id`)
	if( SQL_ERROR == SqlStmt_Prepare(stmt, "SELECT c.`account_id`, c.`char_id`, c.`name` FROM `%s` c LEFT JOIN `%s` f ON f.`friend_account` = c.`account_id` AND f.`friend_id` = c.`char_id` WHERE f.`char_id`=? LIMIT %d", charserv_table(char_table), charserv_table(friend_table), MAX_FRIENDS)
	||	SQL_ERROR == SqlStmt_BindParam(stmt, 0, SQLDT_INT, &char_id, 0)
	||	SQL_ERROR == SqlStmt_Execute(stmt)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 0, SQLDT_INT,    &tmp_friend.account_id, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 1, SQLDT_INT,    &tmp_friend.char_id, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 2, SQLDT_STRING, &tmp_friend.name, sizeof(tmp_friend.name), NULL, NULL) )
		SqlStmt_ShowDebug(stmt);

	for( i = 0; i < MAX_FRIENDS && SQL_SUCCESS == SqlStmt_NextRow(stmt); ++i )
		memcpy(&p->friends[i], &tmp_friend, sizeof(tmp_friend));
	StringBuf_AppendStr(&msg_buf, " friends");

#ifdef HOTKEY_SAVING
	//read hotkeys
	//`hotkey` (`char_id`, `hotkey`, `type`, `itemskill_id`, `skill_lvl`
	if( SQL_ERROR == SqlStmt_Prepare(stmt, "SELECT `hotkey`, `type`, `itemskill_id`, `skill_lvl` FROM `%s` WHERE `char_id`=?", charserv_table(hotkey_table))
	||	SQL_ERROR == SqlStmt_BindParam(stmt, 0, SQLDT_INT, &char_id, 0)
	||	SQL_ERROR == SqlStmt_Execute(stmt)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 0, SQLDT_INT,    &hotkey_num, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 1, SQLDT_UCHAR,  &tmp_hotkey.type, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 2, SQLDT_UINT,   &tmp_hotkey.id, 0, NULL, NULL)
	||	SQL_ERROR == SqlStmt_BindColumn(stmt, 3, SQLDT_USHORT, &tmp_hotkey.lv, 0, NULL, NULL) )
		SqlStmt_ShowDebug(stmt);

	while( SQL_SUCCESS == SqlStmt_NextRow(stmt) )
	{
		if( hotkey_num >= 0 && hotkey_num < MAX_HOTKEYS )
			memcpy(&p->hotkeys[hotkey_num], &tmp_hotkey, sizeof(tmp_hotkey));
		else
			ShowWarning("mmo_char_fromsql: ignoring invalid hotkey (hotkey=%d,type=%u,id=%u,lv=%u) of character %s (AID=%d,CID=%d)\n", hotkey_num, tmp_hotkey.type, tmp_hotkey.id, tmp_hotkey.lv, p->name, p->account_id, p->char_id);
	}
	StringBuf_AppendStr(&msg_buf, " hotkeys");
#endif

	/* Mercenary Owner DataBase */
	mercenary_owner_fromsql(char_id, p);
	StringBuf_AppendStr(&msg_buf, " mercenary");


	if (charserv_config.save_log) ShowInfo("Loaded char (%d - %s): %s\n", char_id, p->name, StringBuf_Value(&msg_buf));	//ok. all data load successfuly!
	SqlStmt_Free(stmt);
	StringBuf_Destroy(&buf);

	cp = (struct mmo_charstatus *)idb_ensure(char_db_, char_id, char_create_charstatus);
	memcpy(cp, p, sizeof(struct mmo_charstatus));
	StringBuf_Destroy(&msg_buf);
	return 1;
}

//==========================================================================================================
int char_mmo_sql_init(void) {
	char_db_= idb_alloc(DB_OPT_RELEASE_DATA);

	ShowStatus("Characters per Account: '%d'.\n", charserv_config.char_config.char_per_account);

	//the 'set offline' part is now in check_login_conn ...
	//if the server connects to loginserver
	//it will dc all off players
	//and send the loginserver the new state....

	// Force all users offline in sql when starting char-server
	// (useful when servers crashs and don't clean the database)
	char_set_all_offline_sql();

	return 0;
}

//-----------------------------------
// Function to change chararcter's names
//-----------------------------------
int char_rename_char_sql(struct char_session_data *sd, uint32 char_id)
{
	struct mmo_charstatus char_dat;
	char esc_name[NAME_LENGTH*2+1];

	if( sd->new_name[0] == 0 ) // Not ready for rename
		return 2;

	if( !char_mmo_char_fromsql(char_id, &char_dat, false) ) // Only the short data is needed.
		return 2;

	if( char_dat.rename == 0 )
		return 1;

	Sql_EscapeStringLen(sql_handle, esc_name, sd->new_name, strnlen(sd->new_name, NAME_LENGTH));

	// check if the char exist
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT 1 FROM `%s` WHERE `name` LIKE '%s' LIMIT 1", charserv_table(char_table), esc_name) )
	{
		Sql_ShowDebug(sql_handle);
		return 4;
	}

	if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `name` = '%s', `rename` = '%d' WHERE `char_id` = '%d'", charserv_table(char_table), esc_name, --char_dat.rename, char_id) )
	{
		Sql_ShowDebug(sql_handle);
		return 3;
	}

	// Change character's name into guild_db.
	if( char_dat.guild_id )
		inter_guild_charname_changed(char_dat.guild_id, sd->account_id, char_id, sd->new_name);

	safestrncpy(char_dat.name, sd->new_name, NAME_LENGTH);
	memset(sd->new_name,0,sizeof(sd->new_name));

	// log change
	if( charserv_config.log_char )
	{
		if( SQL_ERROR == Sql_Query(sql_handle, "INSERT INTO `%s` (`time`, `char_msg`,`account_id`,`char_num`,`name`,`str`,`agi`,`vit`,`int`,`dex`,`luk`,`hair`,`hair_color`)"
			"VALUES (NOW(), '%s', '%d', '%d', '%s', '0', '0', '0', '0', '0', '0', '0', '0')",
			charserv_table(charlog_table), "change char name", sd->account_id, char_dat.slot, esc_name) )
			Sql_ShowDebug(sql_handle);
	}

	return 0;
}

int char_check_char_name(char * name, char * esc_name)
{
	int i;

	// check length of character name
	if( name[0] == '\0' )
		return -2; // empty character name
	/**
	 * The client does not allow you to create names with less than 4 characters, however,
	 * the use of WPE can bypass this, and this fixes the exploit.
	 **/
	if( strlen( name ) < 4 )
		return -2;
	// check content of character name
	if( remove_control_chars(name) )
		return -2; // control chars in name

	// check for reserved names
	if( strcmpi(name, charserv_config.wisp_server_name) == 0 )
		return -1; // nick reserved for internal server messages

	// Check Authorised letters/symbols in the name of the character
	if( charserv_config.char_config.char_name_option == 1 )
	{ // only letters/symbols in char_name_letters are authorised
		for( i = 0; i < NAME_LENGTH && name[i]; i++ )
			if( strchr(charserv_config.char_config.char_name_letters, name[i]) == NULL )
				return -2;
	}
	else if( charserv_config.char_config.char_name_option == 2 )
	{ // letters/symbols in char_name_letters are forbidden
		for( i = 0; i < NAME_LENGTH && name[i]; i++ )
			if( strchr(charserv_config.char_config.char_name_letters, name[i]) != NULL )
				return -2;
	}
	if( charserv_config.char_config.name_ignoring_case ) {
		if( SQL_ERROR == Sql_Query(sql_handle, "SELECT 1 FROM `%s` WHERE BINARY `name` = '%s' LIMIT 1", charserv_table(char_table), esc_name) ) {
			Sql_ShowDebug(sql_handle);
			return -2;
		}
	} else {
		if( SQL_ERROR == Sql_Query(sql_handle, "SELECT 1 FROM `%s` WHERE `name` = '%s' LIMIT 1", charserv_table(char_table), esc_name) ) {
			Sql_ShowDebug(sql_handle);
			return -2;
		}
	}
	if( Sql_NumRows(sql_handle) > 0 )
		return -1; // name already exists

	return 0;
}

//-----------------------------------
// Function to create a new character
//-----------------------------------
#if PACKETVER >= 20120307
#if PACKETVER >= 20151001
int char_make_new_char_sql(struct char_session_data* sd, char* name_, int slot, int hair_color, int hair_style, short start_job, short unknown, int sex) { // TODO: Unknown byte
#else
int char_make_new_char_sql(struct char_session_data* sd, char* name_, int slot, int hair_color, int hair_style) {
#endif
	int str = 1, agi = 1, vit = 1, int_ = 1, dex = 1, luk = 1;
#else
int char_make_new_char_sql(struct char_session_data* sd, char* name_, int str, int agi, int vit, int int_, int dex, int luk, int slot, int hair_color, int hair_style) {
#endif
	char name[NAME_LENGTH];
	char esc_name[NAME_LENGTH*2+1];
	struct point tmp_start_point[MAX_STARTPOINT];
	struct startitem tmp_start_items[MAX_STARTITEM];
	uint32 char_id;
	int flag, k, start_point_idx = rand() % charserv_config.start_point_count;

	safestrncpy(name, name_, NAME_LENGTH);
	normalize_name(name,TRIM_CHARS);
	Sql_EscapeStringLen(sql_handle, esc_name, name, strnlen(name, NAME_LENGTH));

	memset(tmp_start_point, 0, MAX_STARTPOINT * sizeof(struct point));
	memset(tmp_start_items, 0, MAX_STARTITEM * sizeof(struct startitem));
	memcpy(tmp_start_point, charserv_config.start_point, MAX_STARTPOINT * sizeof(struct point));
	memcpy(tmp_start_items, charserv_config.start_items, MAX_STARTITEM * sizeof(struct startitem));

	flag = char_check_char_name(name,esc_name);
	if( flag < 0 )
		return flag;

	//check other inputs
#if PACKETVER >= 20120307
#if PACKETVER >= 20151001
	switch(sex) {
			case SEX_FEMALE:
				sex = 'F';
				break;
			case SEX_MALE:
				sex = 'M';
				break;
			default:
				sex = 'U';
				break;
	}
#endif
	if(slot < 0 || slot >= sd->char_slots)
#else
	if((slot < 0 || slot >= sd->char_slots) // slots
	|| (str + agi + vit + int_ + dex + luk != 6*5 ) // stats
	|| (str < 1 || str > 9 || agi < 1 || agi > 9 || vit < 1 || vit > 9 || int_ < 1 || int_ > 9 || dex < 1 || dex > 9 || luk < 1 || luk > 9) // individual stat values
	|| (str + int_ != 10 || agi + luk != 10 || vit + dex != 10) ) // pairs
#endif
#if PACKETVER >= 20100413
		return -4; // invalid slot
#else
		return -2; // invalid input
#endif


	// check the number of already existing chars in this account
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT 1 FROM `%s` WHERE `account_id` = '%d'", charserv_table(char_table), sd->account_id) )
		Sql_ShowDebug(sql_handle);
	if( Sql_NumRows(sql_handle) >= sd->char_slots )
		return -2; // character account limit exceeded

	// check char slot
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT 1 FROM `%s` WHERE `account_id` = '%d' AND `char_num` = '%d' LIMIT 1", charserv_table(char_table), sd->account_id, slot) )
		Sql_ShowDebug(sql_handle);
	if( Sql_NumRows(sql_handle) > 0 )
		return -2; // slot already in use

	// validation success, log result
	if (charserv_config.log_char) {
		if( SQL_ERROR == Sql_Query(sql_handle, "INSERT INTO `%s` (`time`, `char_msg`,`account_id`,`char_num`,`name`,`str`,`agi`,`vit`,`int`,`dex`,`luk`,`hair`,`hair_color`)"
			"VALUES (NOW(), '%s', '%d', '%d', '%s', '%d', '%d', '%d', '%d', '%d', '%d', '%d', '%d')",
			charserv_table(charlog_table), "make new char", sd->account_id, slot, esc_name, str, agi, vit, int_, dex, luk, hair_style, hair_color) )
			Sql_ShowDebug(sql_handle);
	}

#if PACKETVER >= 20151001
	if (start_job != JOB_NOVICE && start_job != JOB_SUMMONER)
		return -2; // Invalid job

	// Check for Doram based information.
	if (start_job == JOB_SUMMONER) { // Check for just this job for now.
		memset(tmp_start_point, 0, MAX_STARTPOINT * sizeof(struct point));
		memset(tmp_start_items, 0, MAX_STARTITEM * sizeof(struct startitem));
		memcpy(tmp_start_point, charserv_config.start_point_doram, MAX_STARTPOINT * sizeof(struct point));
		memcpy(tmp_start_items, charserv_config.start_items_doram, MAX_STARTITEM * sizeof(struct startitem));
		start_point_idx = rand() % charserv_config.start_point_count_doram;
	}
#endif

	//Insert the new char entry to the database
#if PACKETVER >= 20151001
	if( SQL_ERROR == Sql_Query(sql_handle, "INSERT INTO `%s` (`account_id`, `char_num`, `name`, `class`, `zeny`, `status_point`, `str`, `agi`, `vit`, `int`, `dex`, `luk`, `max_hp`, `hp`,"
		"`max_sp`, `sp`, `hair`, `hair_color`, `last_map`, `last_x`, `last_y`, `save_map`, `save_x`, `save_y`, `sex`) VALUES ("
		"'%d', '%d', '%s', '%d', '%d',  '%d', '%d', '%d', '%d', '%d', '%d', '%d', '%d', '%d', '%d', '%d', '%d', '%d', '%s', '%d', '%d', '%s', '%d', '%d', '%c')",
		schema_config.char_db, sd->account_id , slot, esc_name, start_job, charserv_config.start_zeny, 48, str, agi, vit, int_, dex, luk,
		(40 * (100 + vit)/100) , (40 * (100 + vit)/100 ),  (11 * (100 + int_)/100), (11 * (100 + int_)/100), hair_style, hair_color,
		mapindex_id2name(tmp_start_point[start_point_idx].map), tmp_start_point[start_point_idx].x, tmp_start_point[start_point_idx].y, mapindex_id2name(tmp_start_point[start_point_idx].map), tmp_start_point[start_point_idx].x, tmp_start_point[start_point_idx].y, sex) )
#elif PACKETVER >= 20120307
	if( SQL_ERROR == Sql_Query(sql_handle, "INSERT INTO `%s` (`account_id`, `char_num`, `name`, `zeny`, `status_point`, `str`, `agi`, `vit`, `int`, `dex`, `luk`, `max_hp`, `hp`,"
		"`max_sp`, `sp`, `hair`, `hair_color`, `last_map`, `last_x`, `last_y`, `save_map`, `save_x`, `save_y`) VALUES ("
		"'%d', '%d', '%s', '%d',  '%d', '%d', '%d', '%d', '%d', '%d', '%d', '%d', '%d', '%d', '%d', '%d', '%d', '%s', '%d', '%d', '%s', '%d', '%d')",
		charserv_table(char_table), sd->account_id , slot, esc_name, charserv_config.start_zeny, 48, str, agi, vit, int_, dex, luk,
		(40 * (100 + vit)/100) , (40 * (100 + vit)/100 ),  (11 * (100 + int_)/100), (11 * (100 + int_)/100), hair_style, hair_color,
		mapindex_id2name(tmp_start_point[start_point_idx].map), tmp_start_point[start_point_idx].x, tmp_start_point[start_point_idx].y, mapindex_id2name(tmp_start_point[start_point_idx].map), tmp_start_point[start_point_idx].x, tmp_start_point[start_point_idx].y) )
#else
	if( SQL_ERROR == Sql_Query(sql_handle, "INSERT INTO `%s` (`account_id`, `char_num`, `name`, `zeny`, `str`, `agi`, `vit`, `int`, `dex`, `luk`, `max_hp`, `hp`,"
		"`max_sp`, `sp`, `hair`, `hair_color`, `last_map`, `last_x`, `last_y`, `save_map`, `save_x`, `save_y`) VALUES ("
		"'%d', '%d', '%s', '%d',  '%d', '%d', '%d', '%d', '%d', '%d', '%d', '%d', '%d', '%d', '%d', '%d', '%s', '%d', '%d', '%s', '%d', '%d')",
		charserv_table(char_table), sd->account_id , slot, esc_name, charserv_config.start_zeny, str, agi, vit, int_, dex, luk,
		(40 * (100 + vit)/100) , (40 * (100 + vit)/100 ),  (11 * (100 + int_)/100), (11 * (100 + int_)/100), hair_style, hair_color,
		mapindex_id2name(tmp_start_point[start_point_idx].map), tmp_start_point[start_point_idx].x, tmp_start_point[start_point_idx].y, mapindex_id2name(tmp_start_point[start_point_idx].map), tmp_start_point[start_point_idx].x, tmp_start_point[start_point_idx].y) )
#endif
	{
		Sql_ShowDebug(sql_handle);
		return -2; //No, stop the procedure!
	}

	//Retrieve the newly auto-generated char id
	char_id = (int)Sql_LastInsertId(sql_handle);
	//Give the char the default items
	for (k = 0; k <= MAX_STARTITEM && charserv_config.start_items[k].nameid != 0; k ++) {
		if( SQL_ERROR == Sql_Query(sql_handle, "INSERT INTO `%s` (`char_id`,`nameid`, `amount`, `equip`, `identify`) VALUES ('%d', '%hu', '%hu', '%hu', '%d')", charserv_table(inventory_table), char_id, charserv_config.start_items[k].nameid, charserv_config.start_items[k].amount, charserv_config.start_items[k].pos, 1) )
			Sql_ShowDebug(sql_handle);
	}

	ShowInfo("Created char: account: %d, char: %d, slot: %d, name: %s\n", sd->account_id, char_id, slot, name);
	return char_id;
}

/*----------------------------------------------------------------------------------------------------------*/
/* Divorce Players */
/*----------------------------------------------------------------------------------------------------------*/
int char_divorce_char_sql(int partner_id1, int partner_id2){
	if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `partner_id`='0' WHERE `char_id`='%d' OR `char_id`='%d' LIMIT 2", charserv_table(char_table), partner_id1, partner_id2) )
		Sql_ShowDebug(sql_handle);
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE (`nameid`='%hu' OR `nameid`='%hu') AND (`char_id`='%d' OR `char_id`='%d') LIMIT 2", charserv_table(inventory_table), WEDDING_RING_M, WEDDING_RING_F, partner_id1, partner_id2) )
		Sql_ShowDebug(sql_handle);
	chmapif_send_ackdivorce(partner_id1, partner_id2);
	return 0;
}

/*----------------------------------------------------------------------------------------------------------*/
/* Delete char - davidsiaw */
/*----------------------------------------------------------------------------------------------------------*/
/* Returns 0 if successful
 * Returns < 0 for error
 */
int char_delete_char_sql(uint32 char_id){
	char name[NAME_LENGTH];
	char esc_name[NAME_LENGTH*2+1]; //Name needs be escaped.
	uint32 account_id;
	int party_id, guild_id, hom_id, base_level, partner_id, father_id, mother_id, elemental_id;
	char *data;
	size_t len;

	if (SQL_ERROR == Sql_Query(sql_handle, "SELECT `name`,`account_id`,`party_id`,`guild_id`,`base_level`,`homun_id`,`partner_id`,`father`,`mother`,`elemental_id` FROM `%s` WHERE `char_id`='%d'", charserv_table(char_table), char_id))
		Sql_ShowDebug(sql_handle);

	if( SQL_SUCCESS != Sql_NextRow(sql_handle) )
	{
		ShowError("delete_char_sql: Unable to fetch character data, deletion aborted.\n");
		Sql_FreeResult(sql_handle);
		return -1;
	}

	Sql_GetData(sql_handle, 0, &data, &len); safestrncpy(name, data, NAME_LENGTH);
	Sql_GetData(sql_handle, 1, &data, NULL); account_id = atoi(data);
	Sql_GetData(sql_handle, 2, &data, NULL); party_id = atoi(data);
	Sql_GetData(sql_handle, 3, &data, NULL); guild_id = atoi(data);
	Sql_GetData(sql_handle, 4, &data, NULL); base_level = atoi(data);
	Sql_GetData(sql_handle, 5, &data, NULL); hom_id = atoi(data);
	Sql_GetData(sql_handle, 6, &data, NULL); partner_id = atoi(data);
	Sql_GetData(sql_handle, 7, &data, NULL); father_id = atoi(data);
	Sql_GetData(sql_handle, 8, &data, NULL); mother_id = atoi(data);
	Sql_GetData(sql_handle, 9, &data, NULL); elemental_id = atoi(data);

	Sql_EscapeStringLen(sql_handle, esc_name, name, zmin(len, NAME_LENGTH));
	Sql_FreeResult(sql_handle);

	//check for config char del condition [Lupus]
	// TODO: Move this out to packet processing (0x68/0x1fb).
	if( ( charserv_config.char_config.char_del_level > 0 && base_level >= charserv_config.char_config.char_del_level )
	 || ( charserv_config.char_config.char_del_level < 0 && base_level <= -charserv_config.char_config.char_del_level )
	) {
			ShowInfo("Char deletion aborted: %s, BaseLevel: %i\n", name, base_level);
			return -1;
	}

	/* Divorce [Wizputer] */
	if( partner_id )
		char_divorce_char_sql(char_id, partner_id);

	/* De-addopt [Zephyrus] */
	if( father_id || mother_id )
	{ // Char is Baby
		unsigned char buf[64];

		if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `child`='0' WHERE `char_id`='%d' OR `char_id`='%d'", charserv_table(char_table), father_id, mother_id) )
			Sql_ShowDebug(sql_handle);
		if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `id` = '410'AND (`char_id`='%d' OR `char_id`='%d')", charserv_table(skill_table), father_id, mother_id) )
			Sql_ShowDebug(sql_handle);

		WBUFW(buf,0) = 0x2b25;
		WBUFL(buf,2) = father_id;
		WBUFL(buf,6) = mother_id;
		WBUFL(buf,10) = char_id; // Baby
		chmapif_sendall(buf,14);
	}

	//Make the character leave the party [Skotlex]
	if (party_id)
		inter_party_leave(party_id, account_id, char_id);

	/* delete char's pet */
	//Delete the hatched pet if you have one...
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `char_id`='%d' AND `incubate` = '0'", charserv_table(pet_table), char_id) )
		Sql_ShowDebug(sql_handle);

	//Delete all pets that are stored in eggs (inventory + cart)
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` USING `%s` JOIN `%s` ON `pet_id` = `card1`|`card2`<<16 WHERE `%s`.char_id = '%d' AND card0 = 256", charserv_table(pet_table), charserv_table(pet_table), charserv_table(inventory_table), charserv_table(inventory_table), char_id) )
		Sql_ShowDebug(sql_handle);
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` USING `%s` JOIN `%s` ON `pet_id` = `card1`|`card2`<<16 WHERE `%s`.char_id = '%d' AND card0 = 256", charserv_table(pet_table), charserv_table(pet_table), charserv_table(cart_table), charserv_table(cart_table), char_id) )
		Sql_ShowDebug(sql_handle);

	/* remove homunculus */
	if( hom_id )
		mapif_homunculus_delete(hom_id);

	/* remove elemental */
	if (elemental_id)
		mapif_elemental_delete(elemental_id);

	/* remove mercenary data */
	mercenary_owner_delete(char_id);

	/* delete char's friends list */
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `char_id` = '%d'", charserv_table(friend_table), char_id) )
		Sql_ShowDebug(sql_handle);

	/* delete char from other's friend list */
	//NOTE: Won't this cause problems for people who are already online? [Skotlex]
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `friend_id` = '%d'", charserv_table(friend_table), char_id) )
		Sql_ShowDebug(sql_handle);

#ifdef HOTKEY_SAVING
	/* delete hotkeys */
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `char_id`='%d'", charserv_table(hotkey_table), char_id) )
		Sql_ShowDebug(sql_handle);
#endif

	/* delete inventory */
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `char_id`='%d'", charserv_table(inventory_table), char_id) )
		Sql_ShowDebug(sql_handle);

	/* delete cart inventory */
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `char_id`='%d'", charserv_table(cart_table), char_id) )
		Sql_ShowDebug(sql_handle);

	/* delete memo areas */
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `char_id`='%d'", charserv_table(memo_table), char_id) )
		Sql_ShowDebug(sql_handle);

	/* delete character registry */
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `char_id`='%d'", charserv_table(char_reg_str_table), char_id) )
		Sql_ShowDebug(sql_handle);
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `char_id`='%d'", charserv_table(char_reg_num_table), char_id) )
		Sql_ShowDebug(sql_handle);

	/* delete skills */
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `char_id`='%d'", charserv_table(skill_table), char_id) )
		Sql_ShowDebug(sql_handle);

	/* delete mails (only received) */
	if (SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `dest_id`='%d'", charserv_table(mail_table), char_id))
		Sql_ShowDebug(sql_handle);

#ifdef ENABLE_SC_SAVING
	/* status changes */
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `account_id` = '%d' AND `char_id`='%d'", charserv_table(scdata_table), account_id, char_id) )
		Sql_ShowDebug(sql_handle);
#endif

	/* bonus_scripts */
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `char_id` = '%d'", charserv_table(bonus_script_table), char_id) )
		Sql_ShowDebug(sql_handle);

	if (charserv_config.log_char) {
		if( SQL_ERROR == Sql_Query(sql_handle, "INSERT INTO `%s`(`time`, `account_id`,`char_num`,`char_msg`,`name`) VALUES (NOW(), '%d', '%d', 'Deleted char (CID %d)', '%s')",
			charserv_table(charlog_table), account_id, 0, char_id, esc_name) )
			Sql_ShowDebug(sql_handle);
	}

	/* delete character */
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `char_id`='%d'", charserv_table(char_table), char_id) )
		Sql_ShowDebug(sql_handle);

	/* No need as we used inter_guild_leave [Skotlex]
	// Also delete info from guildtables.
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `char_id`='%d'", guild_member_db, char_id) )
		Sql_ShowDebug(sql_handle);
	*/

	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `guild_id` FROM `%s` WHERE `char_id` = '%d'", charserv_table(guild_table), char_id) )
		Sql_ShowDebug(sql_handle);
	else if( Sql_NumRows(sql_handle) > 0 )
		mapif_parse_BreakGuild(0,guild_id);
	else if( guild_id )
		inter_guild_leave(guild_id, account_id, char_id);// Leave your guild.
	return 0;
}

/**
 * This function parse all map-serv attached to this char-serv and increase user count
 * @return numbers of total users
 */
int char_count_users(void)
{
	int i, users;

	users = 0;
	for(i = 0; i < ARRAYLENGTH(map_server); i++) {
		if (map_server[i].fd > 0) {
			users += map_server[i].users;
		}
	}
	return users;
}

// Writes char data to the buffer in the format used by the client.
// Used in packets 0x6b (chars info) and 0x6d (new char info)
// Returns the size
int char_mmo_char_tobuf(uint8* buffer, struct mmo_charstatus* p)
{
	unsigned short offset = 0;
	uint8* buf;

	if( buffer == NULL || p == NULL )
		return 0;

	buf = WBUFP(buffer,0);
	WBUFL(buf,0) = p->char_id;
	WBUFL(buf,4) = umin(p->base_exp, INT32_MAX);
	WBUFL(buf,8) = p->zeny;
	WBUFL(buf,12) = umin(p->job_exp, INT32_MAX);
	WBUFL(buf,16) = p->job_level;
	WBUFL(buf,20) = 0; // probably opt1
	WBUFL(buf,24) = 0; // probably opt2
	WBUFL(buf,28) = p->option;
	WBUFL(buf,32) = p->karma;
	WBUFL(buf,36) = p->manner;
	WBUFW(buf,40) = umin(p->status_point, INT16_MAX);
	WBUFL(buf,42) = p->hp;
	WBUFL(buf,46) = p->max_hp;
	offset+=4;
	buf = WBUFP(buffer,offset);
	WBUFW(buf,46) = min(p->sp, INT16_MAX);
	WBUFW(buf,48) = min(p->max_sp, INT16_MAX);
	WBUFW(buf,50) = DEFAULT_WALK_SPEED; // p->speed;
	WBUFW(buf,52) = p->class_;
	WBUFW(buf,54) = p->hair;

#if PACKETVER >= 20141022
	WBUFW(buf,56) = p->body;
	offset+=2;
	buf = WBUFP(buffer,offset);
#endif

	//When the weapon is sent and your option is riding, the client crashes on login!?
	WBUFW(buf,56) = p->option&(0x20|0x80000|0x100000|0x200000|0x400000|0x800000|0x1000000|0x2000000|0x4000000|0x8000000) ? 0 : p->weapon;

	WBUFW(buf,58) = p->base_level;
	WBUFW(buf,60) = umin(p->skill_point, INT16_MAX);
	WBUFW(buf,62) = p->head_bottom;
	WBUFW(buf,64) = p->shield;
	WBUFW(buf,66) = p->head_top;
	WBUFW(buf,68) = p->head_mid;
	WBUFW(buf,70) = p->hair_color;
	WBUFW(buf,72) = p->clothes_color;
	memcpy(WBUFP(buf,74), p->name, NAME_LENGTH);
	WBUFB(buf,98) = (unsigned char)u16min(p->str, UINT8_MAX);
	WBUFB(buf,99) = (unsigned char)u16min(p->agi, UINT8_MAX);
	WBUFB(buf,100) = (unsigned char)u16min(p->vit, UINT8_MAX);
	WBUFB(buf,101) = (unsigned char)u16min(p->int_, UINT8_MAX);
	WBUFB(buf,102) = (unsigned char)u16min(p->dex, UINT8_MAX);
	WBUFB(buf,103) = (unsigned char)u16min(p->luk, UINT8_MAX);
	WBUFW(buf,104) = p->slot;
	WBUFW(buf,106) = ( p->rename > 0 ) ? 0 : 1;
	offset += 2;
#if (PACKETVER >= 20100720 && PACKETVER <= 20100727) || PACKETVER >= 20100803
	mapindex_getmapname_ext(mapindex_id2name(p->last_point.map), (char*)WBUFP(buf,108));
	offset += MAP_NAME_LENGTH_EXT;
#endif
#if PACKETVER >= 20100803
#if (PACKETVER > 20130000 && PACKETVER < 20141016) || (PACKETVER >= 20150826 && PACKETVER < 20151001) || PACKETVER >= 20151104
	WBUFL(buf,124) = (p->delete_date?TOL(p->delete_date-time(NULL)):0);
#else
	WBUFL(buf,124) = TOL(p->delete_date);
#endif
	offset += 4;
#endif
#if PACKETVER >= 20110111
	WBUFL(buf,128) = p->robe;
	offset += 4;
#endif
#if PACKETVER != 20111116 //2011-11-16 wants 136, ask gravity.
	#if PACKETVER >= 20110928
		// change slot feature (0 = disabled, otherwise enabled)
		if( (charserv_config.charmove_config.char_move_enabled)==0 )
			WBUFL(buf,132) = 0;
		else if( charserv_config.charmove_config.char_moves_unlimited )
			WBUFL(buf,132) = 1;
		else
			WBUFL(buf,132) = max( 0, (int)p->character_moves );
		offset += 4;
	#endif
	#if PACKETVER >= 20111025
		WBUFL(buf,136) = ( p->rename > 0 ) ? 1 : 0;  // (0 = disabled, otherwise displays "Add-Ons" sidebar)
		offset += 4;
	#endif
	#if PACKETVER >= 20141016
		WBUFB(buf,140) = p->sex;// sex - (0 = female, 1 = male, 99 = logindefined)
		offset += 1;
	#endif
#endif

	return 106+offset;
}


int char_married(int pl1, int pl2)
{
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `partner_id` FROM `%s` WHERE `char_id` = '%d'", charserv_table(char_table), pl1) )
		Sql_ShowDebug(sql_handle);
	else if( SQL_SUCCESS == Sql_NextRow(sql_handle) )
	{
		char* data;

		Sql_GetData(sql_handle, 0, &data, NULL);
		if( pl2 == atoi(data) )
		{
			Sql_FreeResult(sql_handle);
			return 1;
		}
	}
	Sql_FreeResult(sql_handle);
	return 0;
}

int char_child(int parent_id, int child_id)
{
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `child` FROM `%s` WHERE `char_id` = '%d'", charserv_table(char_table), parent_id) )
		Sql_ShowDebug(sql_handle);
	else if( SQL_SUCCESS == Sql_NextRow(sql_handle) )
	{
		char* data;

		Sql_GetData(sql_handle, 0, &data, NULL);
		if( child_id == atoi(data) )
		{
			Sql_FreeResult(sql_handle);
			return 1;
		}
	}
	Sql_FreeResult(sql_handle);
	return 0;
}

int char_family(int cid1, int cid2, int cid3)
{
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `char_id`,`partner_id`,`child` FROM `%s` WHERE `char_id` IN ('%d','%d','%d')", charserv_table(char_table), cid1, cid2, cid3) )
		Sql_ShowDebug(sql_handle);
	else while( SQL_SUCCESS == Sql_NextRow(sql_handle) )
	{
		int charid;
		int partnerid;
		int childid;
		char* data;

		Sql_GetData(sql_handle, 0, &data, NULL); charid = atoi(data);
		Sql_GetData(sql_handle, 1, &data, NULL); partnerid = atoi(data);
		Sql_GetData(sql_handle, 2, &data, NULL); childid = atoi(data);

		if( (cid1 == charid    && ((cid2 == partnerid && cid3 == childid  ) || (cid2 == childid   && cid3 == partnerid))) ||
			(cid1 == partnerid && ((cid2 == charid    && cid3 == childid  ) || (cid2 == childid   && cid3 == charid   ))) ||
			(cid1 == childid   && ((cid2 == charid    && cid3 == partnerid) || (cid2 == partnerid && cid3 == charid   ))) )
		{
			Sql_FreeResult(sql_handle);
			return childid;
		}
	}
	Sql_FreeResult(sql_handle);
	return 0;
}

//----------------------------------------------------------------------
// Force disconnection of an online player (with account value) by [Yor]
//----------------------------------------------------------------------
void char_disconnect_player(uint32 account_id)
{
	int i;
	struct char_session_data* sd;

	// disconnect player if online on char-server
	ARR_FIND( 0, fd_max, i, session[i] && (sd = (struct char_session_data*)session[i]->session_data) && sd->account_id == account_id );
	if( i < fd_max )
		set_eof(i);
}

/**
* Set 'flag' value of char_session_data
* @param account_id
* @param value
* @param set True: set the value by using '|= val', False: unset the value by using '&= ~val'
**/
void char_set_session_flag_(int account_id, int val, bool set) {
	int i;
	struct char_session_data* sd;

	ARR_FIND(0, fd_max, i, session[i] && (sd = (struct char_session_data*)session[i]->session_data) && sd->account_id == account_id);
	if (i < fd_max) {
		if (set)
			sd->flag |= val;
		else
			sd->flag &= ~val;
	}
}

void char_auth_ok(int fd, struct char_session_data *sd) {
	struct online_char_data* character;

	if( (character = (struct online_char_data*)idb_get(online_char_db, sd->account_id)) != NULL )
	{	// check if character is not online already. [Skotlex]
		if (character->server > -1)
		{	//Character already online. KICK KICK KICK
			mapif_disconnectplayer(map_server[character->server].fd, character->account_id, character->char_id, 2);
			if (character->waiting_disconnect == INVALID_TIMER)
				character->waiting_disconnect = add_timer(gettick()+20000, char_chardb_waiting_disconnect, character->account_id, 0);
			chclif_send_auth_result(fd,8);
			return;
		}
		if (character->fd >= 0 && character->fd != fd)
		{	//There's already a connection from this account that hasn't picked a char yet.
			chclif_send_auth_result(fd,8);
			return;
		}
		character->fd = fd;
	}

	chlogif_send_reqaccdata(login_fd,sd); // request account data

	// mark session as 'authed'
	sd->auth = true;

	// set char online on charserver
	char_set_charselect(sd->account_id);

	// continues when account data is received...
}

void char_read_fame_list(void)
{
	int i;
	char* data;
	size_t len;

	// Empty ranking lists
	memset(smith_fame_list, 0, sizeof(smith_fame_list));
	memset(chemist_fame_list, 0, sizeof(chemist_fame_list));
	memset(taekwon_fame_list, 0, sizeof(taekwon_fame_list));
	// Build Blacksmith ranking list
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `char_id`,`fame`,`name` FROM `%s` WHERE `fame`>0 AND (`class`='%d' OR `class`='%d' OR `class`='%d' OR `class`='%d' OR `class`='%d' OR `class`='%d') ORDER BY `fame` DESC LIMIT 0,%d", charserv_table(char_table), JOB_BLACKSMITH, JOB_WHITESMITH, JOB_BABY_BLACKSMITH, JOB_MECHANIC, JOB_MECHANIC_T, JOB_BABY_MECHANIC, fame_list_size_smith) )
		Sql_ShowDebug(sql_handle);
	for( i = 0; i < fame_list_size_smith && SQL_SUCCESS == Sql_NextRow(sql_handle); ++i )
	{
		// char_id
		Sql_GetData(sql_handle, 0, &data, NULL);
		smith_fame_list[i].id = atoi(data);
		// fame
		Sql_GetData(sql_handle, 1, &data, &len);
		smith_fame_list[i].fame = atoi(data);
		// name
		Sql_GetData(sql_handle, 2, &data, &len);
		memcpy(smith_fame_list[i].name, data, zmin(len, NAME_LENGTH));
	}
	// Build Alchemist ranking list
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `char_id`,`fame`,`name` FROM `%s` WHERE `fame`>0 AND (`class`='%d' OR `class`='%d' OR `class`='%d' OR `class`='%d' OR `class`='%d' OR `class`='%d') ORDER BY `fame` DESC LIMIT 0,%d", charserv_table(char_table), JOB_ALCHEMIST, JOB_CREATOR, JOB_BABY_ALCHEMIST, JOB_GENETIC, JOB_GENETIC_T, JOB_BABY_GENETIC, fame_list_size_chemist) )
		Sql_ShowDebug(sql_handle);
	for( i = 0; i < fame_list_size_chemist && SQL_SUCCESS == Sql_NextRow(sql_handle); ++i )
	{
		// char_id
		Sql_GetData(sql_handle, 0, &data, NULL);
		chemist_fame_list[i].id = atoi(data);
		// fame
		Sql_GetData(sql_handle, 1, &data, &len);
		chemist_fame_list[i].fame = atoi(data);
		// name
		Sql_GetData(sql_handle, 2, &data, &len);
		memcpy(chemist_fame_list[i].name, data, zmin(len, NAME_LENGTH));
	}
	// Build Taekwon ranking list
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `char_id`,`fame`,`name` FROM `%s` WHERE `fame`>0 AND (`class`='%d') ORDER BY `fame` DESC LIMIT 0,%d", charserv_table(char_table), JOB_TAEKWON, fame_list_size_taekwon) )
		Sql_ShowDebug(sql_handle);
	for( i = 0; i < fame_list_size_taekwon && SQL_SUCCESS == Sql_NextRow(sql_handle); ++i )
	{
		// char_id
		Sql_GetData(sql_handle, 0, &data, NULL);
		taekwon_fame_list[i].id = atoi(data);
		// fame
		Sql_GetData(sql_handle, 1, &data, &len);
		taekwon_fame_list[i].fame = atoi(data);
		// name
		Sql_GetData(sql_handle, 2, &data, &len);
		memcpy(taekwon_fame_list[i].name, data, zmin(len, NAME_LENGTH));
	}
	Sql_FreeResult(sql_handle);
}

//Loads a character's name and stores it in the buffer given (must be NAME_LENGTH in size)
//Returns 1 on found, 0 on not found (buffer is filled with Unknown char name)
int char_loadName(uint32 char_id, char* name){
	char* data;
	size_t len;

	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `name` FROM `%s` WHERE `char_id`='%d'", charserv_table(char_table), char_id) )
		Sql_ShowDebug(sql_handle);
	else if( SQL_SUCCESS == Sql_NextRow(sql_handle) )
	{
		Sql_GetData(sql_handle, 0, &data, &len);
		safestrncpy(name, data, NAME_LENGTH);
		return 1;
	}
	else
	{
		safestrncpy(name, charserv_config.char_config.unknown_char_name, NAME_LENGTH);
	}
	return 0;
}

// Searches for the mapserver that has a given map (and optionally ip/port, if not -1).
// If found, returns the server's index in the 'server' array (otherwise returns -1).
int char_search_mapserver(unsigned short map, uint32 ip, uint16 port){
	int i, j;

	for(i = 0; i < ARRAYLENGTH(map_server); i++)
	{
		if (map_server[i].fd > 0
		&& (ip == (uint32)-1 || map_server[i].ip == ip)
		&& (port == (uint16)-1 || map_server[i].port == port))
		{
			for (j = 0; map_server[i].map[j]; j++)
				if (map_server[i].map[j] == map)
					return i;
		}
	}

	return -1;
}

/**
 * Test to know if an IP come from LAN or WAN.
 * @param ip: ip to check if in auth network
 * @return 0 if from wan, or subnet_map_ip if lan
 **/
int char_lan_subnetcheck(uint32 ip){
	int i;
	ARR_FIND( 0, subnet_count, i, (subnet[i].char_ip & subnet[i].mask) == (ip & subnet[i].mask) );
	if( i < subnet_count ) {
		ShowInfo("Subnet check [%u.%u.%u.%u]: Matches "CL_CYAN"%u.%u.%u.%u/%u.%u.%u.%u"CL_RESET"\n", CONVIP(ip), CONVIP(subnet[i].char_ip & subnet[i].mask), CONVIP(subnet[i].mask));
		return subnet[i].map_ip;
	} else {
		ShowInfo("Subnet check [%u.%u.%u.%u]: "CL_CYAN"WAN"CL_RESET"\n", CONVIP(ip));
		return 0;
	}
}

// Console Command Parser [Wizputer]
//FIXME to be remove (moved to cnslif / will be done once map/char/login, all have their cnslif interface ready)
int parse_console(const char* buf){
	return cnslif_parse(buf);
}

#if PACKETVER_SUPPORTS_PINCODE
//------------------------------------------------
//Pincode system
//------------------------------------------------
int char_pincode_compare( int fd, struct char_session_data* sd, char* pin ){
	if( strcmp( sd->pincode, pin ) == 0 ){
		sd->pincode_try = 0;
		return 1;
	}else{
		chclif_pincode_sendstate( fd, sd, PINCODE_WRONG );

		if( charserv_config.pincode_config.pincode_maxtry && ++sd->pincode_try >= charserv_config.pincode_config.pincode_maxtry ){
			chlogif_pincode_notifyLoginPinError( sd->account_id );
		}

		return 0;
	}
}


void char_pincode_decrypt( uint32 userSeed, char* pin ){
	int i;
	char tab[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
	char *buf;
	
	for( i = 1; i < 10; i++ ){
		int pos;
		uint32 multiplier = 0x3498, baseSeed = 0x881234;
		
		userSeed = baseSeed + userSeed * multiplier;
		pos = userSeed % ( i + 1 );
		if( i != pos ){
			tab[i] ^= tab[pos];
			tab[pos] ^= tab[i];
			tab[i] ^= tab[pos];
		}
	}

	buf = (char *)aMalloc( sizeof(char) * ( PINCODE_LENGTH + 1 ) );
	memset( buf, 0, PINCODE_LENGTH + 1 );
	for( i = 0; i < PINCODE_LENGTH; i++ ){
		sprintf( buf + i, "%d", tab[pin[i] - '0'] );
	}
	strcpy( pin, buf );
	aFree( buf );
}
#endif

//------------------------------------------------
//Invoked 15 seconds after mapif_disconnectplayer in case the map server doesn't
//replies/disconnect the player we tried to kick. [Skotlex]
//------------------------------------------------
int char_chardb_waiting_disconnect(int tid, unsigned int tick, int id, intptr_t data)
{
	struct online_char_data* character;
	if ((character = (struct online_char_data*)idb_get(online_char_db, id)) != NULL && character->waiting_disconnect == tid)
	{	//Mark it offline due to timeout.
		character->waiting_disconnect = INVALID_TIMER;
		char_set_char_offline(character->char_id, character->account_id);
	}
	return 0;
}

/**
 * @see DBApply
 */
static int char_online_data_cleanup_sub(DBKey key, DBData *data, va_list ap)
{
	struct online_char_data *character= (struct online_char_data *)db_data2ptr(data);
	if (character->fd != -1)
		return 0; //Character still connected
	if (character->server == -2) //Unknown server.. set them offline
		char_set_char_offline(character->char_id, character->account_id);
	if (character->server < 0)
		//Free data from players that have not been online for a while.
		db_remove(online_char_db, key);
	return 0;
}

static int char_online_data_cleanup(int tid, unsigned int tick, int id, intptr_t data){
	online_char_db->foreach(online_char_db, char_online_data_cleanup_sub);
	return 0;
}



//----------------------------------
// Reading Lan Support configuration
// Rewrote: Anvanced subnet check [LuzZza]
//----------------------------------
int char_lan_config_read(const char *lancfgName) {
	FILE *fp;
	int line_num = 0, s_subnet=ARRAYLENGTH(subnet);
	char line[1024], w1[64], w2[64], w3[64], w4[64];

	if((fp = fopen(lancfgName, "r")) == NULL) {
		ShowWarning("LAN Support configuration file is not found: %s\n", lancfgName);
		return 1;
	}

	while(fgets(line, sizeof(line), fp)) {
		line_num++;
		if ((line[0] == '/' && line[1] == '/') || line[0] == '\n' || line[1] == '\n')
			continue;

		if(sscanf(line,"%63[^:]: %63[^:]:%63[^:]:%63[^\r\n]", w1, w2, w3, w4) != 4) {

			ShowWarning("Error syntax of configuration file %s in line %d.\n", lancfgName, line_num);
			continue;
		}

		remove_control_chars(w1);
		remove_control_chars(w2);
		remove_control_chars(w3);
		remove_control_chars(w4);

		if( strcmpi(w1, "subnet") == 0 ){
			if(subnet_count>=s_subnet) { //We skip instead of break in case we want to add other conf in that file.
				ShowError("%s: Too many subnets defined, skipping line %d...\n", lancfgName, line_num);
				continue;
			}
			subnet[subnet_count].mask = str2ip(w2);
			subnet[subnet_count].char_ip = str2ip(w3);
			subnet[subnet_count].map_ip = str2ip(w4);
			if( (subnet[subnet_count].char_ip & subnet[subnet_count].mask) != (subnet[subnet_count].map_ip & subnet[subnet_count].mask) )
			{
				ShowError("%s: Configuration Error: The char server (%s) and map server (%s) belong to different subnetworks!\n", lancfgName, w3, w4);
				continue;
			}
			subnet_count++;
		}
	}

	if( subnet_count > 1 ) /* only useful if there is more than 1 */
		ShowStatus("Read information about %d subnetworks.\n", subnet_count);

	fclose(fp);
	return 0;
}

/**
 * Check if our table are all ok in sqlserver
 * Char tables to check
 * @return 0:fail, 1:success
 */
bool char_checkdb(void){
	int i;
	const char* sqltable[] = {
		charserv_table(char_table), charserv_table(charlog_table), charserv_table(bonus_script_table), charserv_table(cart_table),
		charserv_table(inventory_table), charserv_table(memo_table), charserv_table(scdata_table), charserv_table(skill_table),
		charserv_table(skillcooldown_table), charserv_table(storage_table), charserv_table(friend_table), charserv_table(hotkey_table),
		charserv_table(mail_table), charserv_table(quest_table), charserv_table(pet_table), charserv_table(elemental_table),
		charserv_table(party_table), charserv_table(homunculus_table), charserv_table(homunculus_skill_table), charserv_table(mercenary_table),
		charserv_table(mercenary_owner_table), charserv_table(guild_table), charserv_table(guild_alliance_table),
		charserv_table(guild_castle_table), charserv_table(guild_expulsion_table), charserv_table(guild_member_table),
		charserv_table(guild_position_table), charserv_table(guild_skill_table), charserv_table(guild_storage_table),
		charserv_table(auction_table), charserv_table(ragsrvinfo_table), charserv_table(interlog_table),
		charserv_table(char_reg_num_table), charserv_table(char_reg_str_table),
		charserv_table(acc_reg_num_table), charserv_table(acc_reg_str_table),
	};
	ShowInfo("Start checking DB integrity\n");
	for (i=0; i<ARRAYLENGTH(sqltable); i++){ //check if they all exist and we can acces them in sql-server
		if( SQL_ERROR == Sql_Query(sql_handle, "SELECT 1 FROM `%s` LIMIT 1;", sqltable[i]) )
			return false;
	}
	//checking char_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `char_id`,`account_id`,`char_num`,`name`,`class`,"
		"`base_level`,`job_level`,`base_exp`,`job_exp`,`zeny`,`str`,`agi`,`vit`,`int`,`dex`,`luk`,"
		"`max_hp`,`hp`,`max_sp`,`sp`,`status_point`,`skill_point`,`option`,`karma`,`manner`,`party_id`,"
		"`guild_id`,`pet_id`,`homun_id`,`elemental_id`,`hair`,`hair_color`,`clothes_color`,`weapon`,"
		"`shield`,`head_top`,`head_mid`,`head_bottom`,`robe`,`last_map`,`last_x`,`last_y`,`save_map`,"
		"`save_x`,`save_y`,`partner_id`,`online`,`father`,`mother`,`child`,`fame`,`rename`,`delete_date`,"
		"`moves`,`unban_time`,`font`"
#if PACKETVER >= 20141016
		",`sex`"
#endif
		" FROM `%s`;", charserv_table(char_table)) ){
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking charlog_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `time`,`char_msg`,`account_id`,`char_num`,`name`,"
		"`str`,`agi`,`vit`,`int`,`dex`,`luk`,`hair`,`hair_color`"
		" FROM `%s` LIMIT 1;", charserv_table(charlog_table)) ){
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking char_reg_str_table
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `char_id`,`key`,`index`,`value` from `%s` LIMIT 1;", charserv_table(char_reg_str_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking char_reg_num_table
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `char_id`,`key`,`index`,`value` from `%s` LIMIT 1;", charserv_table(char_reg_num_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking global_acc_reg_str_table
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `account_id`,`key`,`index`,`value` from `%s` LIMIT 1;", charserv_table(acc_reg_str_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking global_acc_reg_num_table
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `account_id`,`key`,`index`,`value` from `%s` LIMIT 1;", charserv_table(acc_reg_num_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking hotkey_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `char_id`,`hotkey`,`type`,`itemskill_id`,`skill_lvl`"
		" FROM `%s` LIMIT 1;", charserv_table(hotkey_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking scdata_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `account_id`,`char_id`,`type`,`tick`,`val1`,`val2`,`val3`,`val4`"
		" FROM `%s` LIMIT 1;", charserv_table(scdata_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking skill_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `char_id`,`id`,`lv`,`flag`"
		" FROM `%s` LIMIT 1;", charserv_table(skill_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking interlog_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `time`,`log`"
		" FROM `%s` LIMIT 1;", charserv_table(interlog_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking memo_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `memo_id`,`char_id`,`map`,`x`,`y`"
		" FROM `%s` LIMIT 1;", charserv_table(memo_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking guild_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `guild_id`,`name`,`char_id`,`master`,`guild_lv`,"
		"`connect_member`,`max_member`,`average_lv`,`exp`,`next_exp`,`skill_point`,`mes1`,`mes2`,"
		"`emblem_len`,`emblem_id`,`emblem_data`"
		" FROM `%s` LIMIT 1;", charserv_table(guild_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking guild_alliance_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `guild_id`,`opposition`,`alliance_id`,`name`"
		" FROM `%s` LIMIT 1;", charserv_table(guild_alliance_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking guild_castle_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `castle_id`,`guild_id`,`economy`,`defense`,`triggerE`,"
		"`triggerD`,`nextTime`,`payTime`,`createTime`,`visibleC`,`visibleG0`,`visibleG1`,`visibleG2`,"
		"`visibleG3`,`visibleG4`,`visibleG5`,`visibleG6`,`visibleG7` "
		" FROM `%s` LIMIT 1;", charserv_table(guild_castle_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking guild_expulsion_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `guild_id`,`account_id`,`name`,`mes`"
		" FROM `%s` LIMIT 1;", charserv_table(guild_expulsion_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking guild_member_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT  `guild_id`,`account_id`,`char_id`,`hair`,"
		"`hair_color`,`gender`,`class`,`lv`,`exp`,`exp_payper`,`online`,`position`,`name`"
		" FROM `%s` LIMIT 1;", charserv_table(guild_member_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking guild_skill_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `guild_id`,`id`,`lv`"
		" FROM `%s` LIMIT 1;", charserv_table(guild_skill_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking guild_position_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `guild_id`,`position`,`name`,`mode`,`exp_mode`"
		" FROM `%s` LIMIT 1;", charserv_table(guild_position_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking party_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `party_id`,`name`,`exp`,`item`,`leader_id`,`leader_char`"
		" FROM `%s` LIMIT 1;", charserv_table(party_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking pet_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `pet_id`,`class`,`name`,`account_id`,`char_id`,`level`,"
		"`egg_id`,`equip`,`intimate`,`hungry`,`rename_flag`,`incubate`"
		" FROM `%s` LIMIT 1;", charserv_table(pet_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking friend_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `char_id`,`friend_account`,`friend_id`"
		" FROM `%s` LIMIT 1;", charserv_table(friend_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking mail_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `id`,`send_name`,`send_id`,`dest_name`,`dest_id`,"
		"`title`,`message`,`time`,`status`,`zeny`,`nameid`,`amount`,`refine`,`attribute`,`identify`,"
		"`card0`,`card1`,`card2`,`card3`,`unique_id`, `bound`"
		" FROM `%s` LIMIT 1;", charserv_table(mail_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking auction_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `auction_id`,`seller_id`,`seller_name`,`buyer_id`,`buyer_name`,"
		" `price`,`buynow`,`hours`,`timestamp`,`nameid`,`item_name`,`type`,`refine`,`attribute`,`card0`,`card1`,"
		" `card2`,`card3`,`unique_id`"
		" FROM `%s` LIMIT 1;", charserv_table(auction_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking quest_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `char_id`,`quest_id`,`state`,`time`,`count1`,`count2`,`count3`"
		" FROM `%s`;", charserv_table(quest_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking homunculus_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `homun_id`,`char_id`,`class`,`prev_class`,`name`,`level`,`exp`,`intimacy`,`hunger`,"
		"`str`,`agi`,`vit`,`int`,`dex`,`luk`,`hp`,`max_hp`,`sp`,`max_sp`,`skill_point`,`alive`,`rename_flag`,`vaporize` "
		" FROM `%s` LIMIT 1;", charserv_table(homunculus_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking skill_homunculus_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `homun_id`,`id`,`lv`"
		" FROM `%s` LIMIT 1;", charserv_table(homunculus_skill_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking mercenary_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `mer_id`,`char_id`,`class`,`hp`,`sp`,`kill_counter`,`life_time`"
		" FROM `%s` LIMIT 1;", charserv_table(mercenary_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking mercenary_owner_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `char_id`,`merc_id`,`arch_calls`,`arch_faith`,"
		"`spear_calls`,`spear_faith`,`sword_calls`,`sword_faith`"
		" FROM `%s` LIMIT 1;", charserv_table(mercenary_owner_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking elemental_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `ele_id`,`char_id`,`class`,`mode`,`hp`,`sp`,`max_hp`,`max_sp`,"
		"`atk1`,`atk2`,`matk`,`aspd`,`def`,`mdef`,`flee`,`hit`,`life_time` "
		" FROM `%s` LIMIT 1;", charserv_table(elemental_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking ragsrvinfo_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `index`,`name`,`exp`,`jexp`,`drop`"
		" FROM `%s` LIMIT 1;", charserv_table(ragsrvinfo_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking skillcooldown_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `account_id`,`char_id`,`skill`,`tick`"
		" FROM `%s` LIMIT 1;", charserv_table(skillcooldown_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking bonus_script_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `char_id`,`script`,`tick`,`flag`,`type`,`icon`"
		" FROM `%s` LIMIT 1;", charserv_table(bonus_script_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking cart_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `id`,`char_id`,`nameid`,`amount`,`equip`,`identify`,`refine`,"
		"`attribute`,`card0`,`card1`,`card2`,`card3`,`expire_time`,`bound`,`unique_id`"
		" FROM `%s` LIMIT 1;", charserv_table(cart_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking inventory_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `id`,`char_id`,`nameid`,`amount`,`equip`,`identify`,`refine`,"
		"`attribute`,`card0`,`card1`,`card2`,`card3`,`expire_time`,`favorite`,`bound`,`unique_id`"
		" FROM `%s` LIMIT 1;", charserv_table(inventory_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking storage_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `id`,`account_id`,`nameid`,`amount`,`equip`,`identify`,`refine`,"
		"`attribute`,`card0`,`card1`,`card2`,`card3`,`expire_time`,`bound`,`unique_id`"
		" FROM `%s` LIMIT 1;", charserv_table(storage_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	//checking guild_storage_db
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `id`,`guild_id`,`nameid`,`amount`,`equip`,`identify`,`refine`,"
		"`attribute`,`card0`,`card1`,`card2`,`card3`,`expire_time`,`bound`,`unique_id`"
		" FROM `%s` LIMIT 1;", charserv_table(guild_storage_table)) ) {
		Sql_ShowDebug(sql_handle);
		return false;
	}
	Sql_FreeResult(sql_handle);
	ShowInfo("DB integrity check finished with success\n");
	return true;
}

/**
 * Get table names
 * @param w1 Config name
 * @param w2 Config value
 **/
static bool char_schema_config_read(const char *w1, const char *w2) {
#define SCHEMA_CONF(var,str) \
	if (!strcmpi(w1,(str))) {\
		StringBuf_PrintfClear(schema_config.var, "%s", w2);\
		return true;\
	}\

	SCHEMA_CONF(acc_reg_num_table, "acc_reg_num_table")
	SCHEMA_CONF(acc_reg_str_table, "acc_reg_str_table")
	SCHEMA_CONF(auction_table, "auction_table")
	SCHEMA_CONF(bonus_script_table, "bonus_script_table")
	SCHEMA_CONF(cart_table, "cart_table")
	SCHEMA_CONF(char_table, "char_table")
	SCHEMA_CONF(charlog_table, "charlog_table")
	SCHEMA_CONF(char_reg_num_table, "char_reg_num_table")
	SCHEMA_CONF(char_reg_str_table, "char_reg_str_table")
	SCHEMA_CONF(elemental_table, "elemental_table")
	SCHEMA_CONF(friend_table, "friend_table")
	SCHEMA_CONF(guild_table, "guild_table")
	SCHEMA_CONF(guild_alliance_table, "guild_alliance_table")
	SCHEMA_CONF(guild_castle_table, "guild_castle_table")
	SCHEMA_CONF(guild_expulsion_table, "guild_expulsion_table")
	SCHEMA_CONF(guild_member_table, "guild_member_table")
	SCHEMA_CONF(guild_position_table, "guild_position_table")
	SCHEMA_CONF(guild_skill_table, "guild_skill_table")
	SCHEMA_CONF(guild_storage_table, "guild_storage_table")
	SCHEMA_CONF(hotkey_table, "hotkey_table")
	SCHEMA_CONF(homunculus_table, "homunculus_table")
	SCHEMA_CONF(homunculus_skill_table, "homunculus_skill_table")
	SCHEMA_CONF(interlog_table, "interlog_table")
	SCHEMA_CONF(inventory_table, "inventory_table")
	SCHEMA_CONF(mail_table, "mail_table")
	SCHEMA_CONF(memo_table, "memo_table")
	SCHEMA_CONF(mercenary_table, "mercenary_table")
	SCHEMA_CONF(mercenary_owner_table, "mercenary_owner_table")
	SCHEMA_CONF(party_table, "party_table")
	SCHEMA_CONF(pet_table, "pet_table")
	SCHEMA_CONF(quest_table, "quest_table")
	SCHEMA_CONF(ragsrvinfo_table, "ragsrvinfo_table")
	SCHEMA_CONF(scdata_table, "scdata_table")
	SCHEMA_CONF(skill_table, "skill_table")
	SCHEMA_CONF(skillcooldown_table, "skillcooldown_table")
	SCHEMA_CONF(storage_table, "storage_table")

	return false;
#undef SCHEMA_CONF
}

//set default config
void char_set_defaults(){
#if PACKETVER_SUPPORTS_PINCODE
	charserv_config.pincode_config.pincode_enabled = true;
	charserv_config.pincode_config.pincode_changetime = 0;
	charserv_config.pincode_config.pincode_maxtry = 3;
	charserv_config.pincode_config.pincode_force = true;
	charserv_config.pincode_config.pincode_allow_repeated = false;
	charserv_config.pincode_config.pincode_allow_sequential = false;
#endif

	charserv_config.charmove_config.char_move_enabled = true;
	charserv_config.charmove_config.char_movetoused = true;
	charserv_config.charmove_config.char_moves_unlimited = false;

	charserv_config.char_config.char_per_account = 0; //Maximum chars per account (default unlimited) [Sirius]
	charserv_config.char_config.char_del_level = 0; //From which level u can delete character [Lupus]
	charserv_config.char_config.char_del_delay = 86400;
#if PACKETVER >= 20100803
	charserv_config.char_config.char_del_option = 2;
#else
	charserv_config.char_config.char_del_option = 1;
#endif

//	charserv_config.userid[24];
//	charserv_config.passwd[24];
//	charserv_config.server_name[20];
	safestrncpy(charserv_config.wisp_server_name,"Server",sizeof(charserv_config.wisp_server_name));
//	charserv_config.login_ip_str[128];
	charserv_config.login_ip = 0;
	charserv_config.login_port = 6900;
//	charserv_config.char_ip_str[128];
	charserv_config.char_ip = 0;
//	charserv_config.bind_ip_str[128];
	charserv_config.bind_ip = INADDR_ANY;
	charserv_config.char_port = 6121;
	charserv_config.char_maintenance = 0;
	charserv_config.char_new = true;
	charserv_config.char_new_display = 0;

	charserv_config.char_config.name_ignoring_case = false; // Allow or not identical name for characters but with a different case by [Yor]
	charserv_config.char_config.char_name_option = 0; // Option to know which letters/symbols are authorised in the name of a character (0: all, 1: only those in char_name_letters, 2: all EXCEPT those in char_name_letters) by [Yor]
	safestrncpy(charserv_config.char_config.unknown_char_name,"Unknown",sizeof(charserv_config.char_config.unknown_char_name)); // Name to use when the requested name cannot be determined
	safestrncpy(charserv_config.char_config.char_name_letters,"",sizeof(charserv_config.char_config.char_name_letters)); // list of letters/symbols allowed (or not) in a character name. by [Yor]

	charserv_config.save_log = 1; // show loading/saving messages
	charserv_config.log_char = 1;	// loggin char or not [devil]
	charserv_config.log_inter = 1;	// loggin inter or not [devil]
	charserv_config.char_check_db =1;

	// See const.h to change the default values
	charserv_config.start_point[0].map = mapindex_name2id(MAP_DEFAULT_NAME); 
	charserv_config.start_point[0].x = MAP_DEFAULT_X;
	charserv_config.start_point[0].y = MAP_DEFAULT_Y;
	charserv_config.start_point_count = 1;

#if PACKETVER >= 20151001
	charserv_config.start_point_doram[0].map = mapindex_name2id(MAP_DEFAULT_NAME);
	charserv_config.start_point_doram[0].x = MAP_DEFAULT_X;
	charserv_config.start_point_doram[0].y = MAP_DEFAULT_Y;
	charserv_config.start_point_count_doram = 1;
#endif

	charserv_config.start_items[0].nameid = 1201;
	charserv_config.start_items[0].amount = 1;
	charserv_config.start_items[0].pos = 2;
	charserv_config.start_items[1].nameid = 2301;
	charserv_config.start_items[1].amount = 1;
	charserv_config.start_items[1].pos = 16;

#if PACKETVER >= 20150101
	charserv_config.start_items_doram[0].nameid = 1681;
	charserv_config.start_items_doram[0].amount = 1;
	charserv_config.start_items_doram[0].pos = 2;
	charserv_config.start_items_doram[1].nameid = 2301;
	charserv_config.start_items_doram[1].amount = 1;
	charserv_config.start_items_doram[1].pos = 16;
#endif

	charserv_config.console = 0;
	charserv_config.max_connect_user = -1;
	charserv_config.gm_allow_group = -1;
	charserv_config.autosave_interval = DEFAULT_AUTOSAVE_INTERVAL;
	charserv_config.start_zeny = 0;
	charserv_config.guild_exp_rate = 100;

	safestrncpy(charserv_config.default_map, "prontera", MAP_NAME_LENGTH);
	charserv_config.default_map_x = 156;
	charserv_config.default_map_y = 191;
}

/**
 * Split start_point configuration values.
 * @param w1_value: Value from w1
 * @param w2_value: Value from w2
 * @param start: Start point reference
 * @param count: Start point count reference
 */
static void char_config_split_startpoint(char *w1_value, char *w2_value, struct point start_point[MAX_STARTPOINT], short *count)
{
	char *lineitem, **fields;
	int i = 0, fields_length = 3 + 1;

	(*count) = 0; // Reset to begin reading

	fields = (char **)aMalloc(fields_length * sizeof(char *));
	if (fields == NULL)
		return; // Failed to allocate memory.
	lineitem = strtok(w2_value, ":");

	while (lineitem != NULL && (*count) < MAX_STARTPOINT) {
		int n = sv_split(lineitem, strlen(lineitem), 0, ',', fields, fields_length, SV_NOESCAPE_NOTERMINATE);

		if (n + 1 < fields_length) {
			ShowDebug("%s: not enough arguments for %s! Skipping...\n", w1_value, lineitem);
			lineitem = strtok(NULL, ":"); //next lineitem
			continue;
		}

		start_point[i].map = mapindex_name2id(fields[1]);
		if (!start_point[i].map) {
			ShowError("Start point %s not found in map-index cache. Setting to default location.\n", start_point[i].map);
			start_point[i].map = mapindex_name2id(MAP_DEFAULT_NAME);
			start_point[i].x = MAP_DEFAULT_X;
			start_point[i].y = MAP_DEFAULT_Y;
		} else {
			start_point[i].x = max(0, atoi(fields[2]));
			start_point[i].y = max(0, atoi(fields[3]));
		}
		(*count)++;

		lineitem = strtok(NULL, ":"); //next lineitem
		i++;
	}
	aFree(fields);
}

/**
 * Split start_items configuration values.
 * @param w1_value: Value from w1
 * @param w2_value: Value from w2
 * @param start: Start item reference
 */
static void char_config_split_startitem(char *w1_value, char *w2_value, struct startitem start_items[MAX_STARTITEM])
{
	char *lineitem, **fields;
	int i = 0, fields_length = 3 + 1;

	fields = (char **)aMalloc(fields_length * sizeof(char *));
	if (fields == NULL)
		return; // Failed to allocate memory.
	lineitem = strtok(w2_value, ":");

	while (lineitem != NULL && i < MAX_STARTITEM) {
		int n = sv_split(lineitem, strlen(lineitem), 0, ',', fields, fields_length, SV_NOESCAPE_NOTERMINATE);

		if (n + 1 < fields_length) {
			ShowDebug("%s: not enough arguments for %s! Skipping...\n", w1_value, lineitem);
			lineitem = strtok(NULL, ":"); //next lineitem
			continue;
		}

		// TODO: Item ID verification
		start_items[i].nameid = max(0, atoi(fields[1]));
		start_items[i].amount = max(0, atoi(fields[2]));
		start_items[i].pos = max(0, atoi(fields[3]));

		lineitem = strtok(NULL, ":"); //next lineitem
		i++;
	}
	aFree(fields);
}

bool char_config_read(const char* cfgName, bool normal){
	char line[1024], w1[1024], w2[1024];
	FILE* fp = fopen(cfgName, "r");

	if (fp == NULL) {
		ShowError("Configuration file not found: %s.\n", cfgName);
		return false;
	}

	while(fgets(line, sizeof(line), fp)) {
		if (line[0] == '/' && line[1] == '/')
			continue;

		if (sscanf(line, "%1023[^:]: %1023[^\r\n]", w1, w2) != 2)
			continue;

		remove_control_chars(w1);
		remove_control_chars(w2);

		// Config that loaded only when server started, not by reloading config file
		if (normal) {
			if (strcmpi(w1, "userid") == 0) {
				safestrncpy(charserv_config.userid, w2, sizeof(charserv_config.userid));
			} else if (strcmpi(w1, "passwd") == 0) {
				safestrncpy(charserv_config.passwd, w2, sizeof(charserv_config.passwd));
			} else if (strcmpi(w1, "server_name") == 0) {
				safestrncpy(charserv_config.server_name, w2, sizeof(charserv_config.server_name));
			} else if (strcmpi(w1, "wisp_server_name") == 0) {
				if (strlen(w2) >= 4) {
					safestrncpy(charserv_config.wisp_server_name, w2, sizeof(charserv_config.wisp_server_name));
				}
			} else if (strcmpi(w1, "login_ip") == 0) {
				charserv_config.login_ip = host2ip(w2);
				if (charserv_config.login_ip) {
					char ip_str[16];
					safestrncpy(charserv_config.login_ip_str, w2, sizeof(charserv_config.login_ip_str));
					ShowStatus("Login server IP address : %s -> %s\n", w2, ip2str(charserv_config.login_ip, ip_str));
				}
			} else if (strcmpi(w1, "login_port") == 0) {
				charserv_config.login_port = atoi(w2);
			} else if (strcmpi(w1, "char_ip") == 0) {
				charserv_config.char_ip = host2ip(w2);
				if (charserv_config.char_ip) {
					char ip_str[16];
					safestrncpy(charserv_config.char_ip_str, w2, sizeof(charserv_config.char_ip_str));
					ShowStatus("Character server IP address : %s -> %s\n", w2, ip2str(charserv_config.char_ip, ip_str));
				}
			} else if (strcmpi(w1, "bind_ip") == 0) {
				charserv_config.bind_ip = host2ip(w2);
				if (charserv_config.bind_ip) {
					char ip_str[16];
					safestrncpy(charserv_config.bind_ip_str, w2, sizeof(charserv_config.bind_ip_str));
					ShowStatus("Character server binding IP address : %s -> %s\n", w2, ip2str(charserv_config.bind_ip, ip_str));
				}
			} else if (strcmpi(w1, "char_port") == 0) {
				charserv_config.char_port = atoi(w2);
			} else if (strcmpi(w1, "console") == 0) {
				charserv_config.console = config_switch(w2);
			}

			// Read table names
			if (char_schema_config_read(w1, w2))
				continue;
		}

		if(strcmpi(w1,"timestamp_format") == 0) {
			safestrncpy(timestamp_format, w2, sizeof(timestamp_format));
		} else if(strcmpi(w1,"console_silent")==0){
			msg_silent = atoi(w2);
			if( msg_silent ) /* only bother if its actually enabled */
				ShowInfo("Console Silent Setting: %d\n", atoi(w2));
		} else if (strcmpi(w1, "console_msg_log") == 0) {
			console_msg_log = atoi(w2);
		} else if (strcmpi(w1, "console_log_filepath") == 0) {
			safestrncpy(console_log_filepath, w2, sizeof(console_log_filepath));
		} else if(strcmpi(w1,"stdout_with_ansisequence")==0){
			stdout_with_ansisequence = config_switch(w2);
		} else if (strcmpi(w1, "char_maintenance") == 0) {
			charserv_config.char_maintenance = atoi(w2);
		} else if (strcmpi(w1, "char_new") == 0) {
			charserv_config.char_new = (bool)atoi(w2);
		} else if (strcmpi(w1, "char_new_display") == 0) {
			charserv_config.char_new_display = atoi(w2);
		} else if (strcmpi(w1, "max_connect_user") == 0) {
			charserv_config.max_connect_user = atoi(w2);
			if (charserv_config.max_connect_user < -1)
				charserv_config.max_connect_user = -1;
		} else if(strcmpi(w1, "gm_allow_group") == 0) {
			charserv_config.gm_allow_group = atoi(w2);
		} else if (strcmpi(w1, "autosave_time") == 0) {
			charserv_config.autosave_interval = atoi(w2)*1000;
			if (charserv_config.autosave_interval <= 0)
				charserv_config.autosave_interval = DEFAULT_AUTOSAVE_INTERVAL;
		} else if (strcmpi(w1, "save_log") == 0) {
			charserv_config.save_log = config_switch(w2);
#ifdef RENEWAL
		} else if (strcmpi(w1, "start_point") == 0) {
#else
		} else if (strcmpi(w1, "start_point_pre") == 0) {
#endif
			char_config_split_startpoint(w1, w2, charserv_config.start_point, &charserv_config.start_point_count);
#if PACKETVER >= 20151001
		} else if (strcmpi(w1, "start_point_doram") == 0) {
			char_config_split_startpoint(w1, w2, charserv_config.start_point_doram, &charserv_config.start_point_count_doram);
#endif
		} else if (strcmpi(w1, "start_zeny") == 0) {
			charserv_config.start_zeny = atoi(w2);
			if (charserv_config.start_zeny < 0)
				charserv_config.start_zeny = 0;
		} else if (strcmpi(w1, "start_items") == 0) {
			char_config_split_startitem(w1, w2, charserv_config.start_items);
#if PACKETVER >= 20151001
		} else if (strcmpi(w1, "start_items_doram") == 0) {
			char_config_split_startitem(w1, w2, charserv_config.start_items_doram);
#endif
		} else if(strcmpi(w1,"log_char")==0) {		//log char or not [devil]
			charserv_config.log_char = atoi(w2);
		} else if (strcmpi(w1, "unknown_char_name") == 0) {
			safestrncpy(charserv_config.char_config.unknown_char_name, w2, sizeof(charserv_config.char_config.unknown_char_name));
			charserv_config.char_config.unknown_char_name[NAME_LENGTH-1] = '\0';
		} else if (strcmpi(w1, "name_ignoring_case") == 0) {
			charserv_config.char_config.name_ignoring_case = (bool)config_switch(w2);
		} else if (strcmpi(w1, "char_name_option") == 0) {
			charserv_config.char_config.char_name_option = atoi(w2);
		} else if (strcmpi(w1, "char_name_letters") == 0) {
			safestrncpy(charserv_config.char_config.char_name_letters, w2, sizeof(charserv_config.char_config.char_name_letters));
		} else if (strcmpi(w1, "char_del_level") == 0) { //disable/enable char deletion by its level condition [Lupus]
			charserv_config.char_config.char_del_level = atoi(w2);
		} else if (strcmpi(w1, "char_del_delay") == 0) {
			charserv_config.char_config.char_del_delay = atoi(w2);
		} else if (strcmpi(w1, "char_del_option") == 0) {
			charserv_config.char_config.char_del_option = atoi(w2);
		} else if(strcmpi(w1,"db_path")==0) {
			safestrncpy(charserv_config.db_path, w2, sizeof(charserv_config.db_path));
		} else if (strcmpi(w1, "fame_list_alchemist") == 0) {
			fame_list_size_chemist = atoi(w2);
			if (fame_list_size_chemist > MAX_FAME_LIST) {
				ShowWarning("Max fame list size is %d (fame_list_alchemist)\n", MAX_FAME_LIST);
				fame_list_size_chemist = MAX_FAME_LIST;
			}
		} else if (strcmpi(w1, "fame_list_blacksmith") == 0) {
			fame_list_size_smith = atoi(w2);
			if (fame_list_size_smith > MAX_FAME_LIST) {
				ShowWarning("Max fame list size is %d (fame_list_blacksmith)\n", MAX_FAME_LIST);
				fame_list_size_smith = MAX_FAME_LIST;
			}
		} else if (strcmpi(w1, "fame_list_taekwon") == 0) {
			fame_list_size_taekwon = atoi(w2);
			if (fame_list_size_taekwon > MAX_FAME_LIST) {
				ShowWarning("Max fame list size is %d (fame_list_taekwon)\n", MAX_FAME_LIST);
				fame_list_size_taekwon = MAX_FAME_LIST;
			}
		} else if (strcmpi(w1, "guild_exp_rate") == 0) {
			charserv_config.guild_exp_rate = atoi(w2);
		} else if (strcmpi(w1, "pincode_enabled") == 0) {
#if PACKETVER_SUPPORTS_PINCODE
			charserv_config.pincode_config.pincode_enabled = config_switch(w2);
		} else if (strcmpi(w1, "pincode_changetime") == 0) {
			charserv_config.pincode_config.pincode_changetime = atoi(w2)*60*60*24;
		} else if (strcmpi(w1, "pincode_maxtry") == 0) {
			charserv_config.pincode_config.pincode_maxtry = atoi(w2);
		} else if (strcmpi(w1, "pincode_force") == 0) {
			charserv_config.pincode_config.pincode_force = config_switch(w2);
		}  else if (strcmpi(w1, "pincode_allow_repeated") == 0) {
			charserv_config.pincode_config.pincode_allow_repeated = config_switch(w2);
		}  else if (strcmpi(w1, "pincode_allow_sequential") == 0) {
			charserv_config.pincode_config.pincode_allow_sequential = config_switch(w2);
#else
			if( config_switch(w2) ) {
				ShowWarning("pincode_enabled requires PACKETVER 20110309 or higher.\n");
			}
#endif
		} else if (strcmpi(w1, "char_move_enabled") == 0) {
			charserv_config.charmove_config.char_move_enabled = config_switch(w2);
		} else if (strcmpi(w1, "char_movetoused") == 0) {
			charserv_config.charmove_config.char_movetoused = config_switch(w2);
		} else if (strcmpi(w1, "char_moves_unlimited") == 0) {
			charserv_config.charmove_config.char_moves_unlimited = config_switch(w2);
		} else if (strcmpi(w1, "char_checkdb") == 0) {
			charserv_config.char_check_db = config_switch(w2);
		} else if (strcmpi(w1, "default_map") == 0) {
			safestrncpy(charserv_config.default_map, w2, MAP_NAME_LENGTH);
		} else if (strcmpi(w1, "default_map_x") == 0) {
			charserv_config.default_map_x = atoi(w2);
		} else if (strcmpi(w1, "default_map_y") == 0) {
			charserv_config.default_map_y = atoi(w2);
		} else if (strcmpi(w1, "import") == 0) {
			char_config_read(w2, normal);
		}
	}
	fclose(fp);

	ShowInfo("Done reading %s.\n", cfgName);
	return true;
}

static void char_schema_config_init(void) {
	// Character related tables
	schema_config.char_table			 = StringBuf_FromStr("char");
	schema_config.charlog_table			 = StringBuf_FromStr("charlog");
	schema_config.bonus_script_table	 = StringBuf_FromStr("bonus_script");
	schema_config.cart_table			 = StringBuf_FromStr("cart_inventory");
	schema_config.inventory_table		 = StringBuf_FromStr("inventory");
	schema_config.memo_table			 = StringBuf_FromStr("memo");
	schema_config.scdata_table			 = StringBuf_FromStr("sc_data");
	schema_config.skill_table			 = StringBuf_FromStr("skill");
	schema_config.skillcooldown_table	 = StringBuf_FromStr("skillcooldown");
	schema_config.storage_table			 = StringBuf_FromStr("storage");
	schema_config.friend_table			 = StringBuf_FromStr("friends");
	schema_config.hotkey_table			 = StringBuf_FromStr("hotkey");
	schema_config.mail_table			 = StringBuf_FromStr("mail");
	schema_config.quest_table			 = StringBuf_FromStr("quest");
	schema_config.pet_table				 = StringBuf_FromStr("pet");
	schema_config.elemental_table		 = StringBuf_FromStr("elemental");
	schema_config.party_table			 = StringBuf_FromStr("party");

	// Homunculus tables
	schema_config.homunculus_table		 = StringBuf_FromStr("homunculus");
	schema_config.homunculus_skill_table = StringBuf_FromStr("skill_homunculus");

	// Mercenary Tables
	schema_config.mercenary_table		 = StringBuf_FromStr("mercenary");
	schema_config.mercenary_owner_table	 = StringBuf_FromStr("mercenary_owner");

	// Guild tables
	schema_config.guild_table			 = StringBuf_FromStr("guild");
	schema_config.guild_alliance_table	 = StringBuf_FromStr("guild_alliance");
	schema_config.guild_castle_table	 = StringBuf_FromStr("guild_castle");
	schema_config.guild_expulsion_table	 = StringBuf_FromStr("guild_expulsion");
	schema_config.guild_member_table	 = StringBuf_FromStr("guild_member");
	schema_config.guild_position_table	 = StringBuf_FromStr("guild_position");
	schema_config.guild_skill_table		 = StringBuf_FromStr("guild_skill");
	schema_config.guild_storage_table	 = StringBuf_FromStr("guild_storage");

	// Other
	schema_config.acc_reg_num_table      = StringBuf_FromStr("acc_reg_num");
	schema_config.acc_reg_str_table      = StringBuf_FromStr("acc_reg_str");
	schema_config.auction_table			 = StringBuf_FromStr("auction");
	schema_config.char_reg_num_table     = StringBuf_FromStr("char_reg_num");
	schema_config.char_reg_str_table     = StringBuf_FromStr("char_reg_str");
	schema_config.ragsrvinfo_table		 = StringBuf_FromStr("ragsrvinfo");
	schema_config.interlog_table		 = StringBuf_FromStr("interlog");
}

static void char_schema_config_final(void) {
	StringBuf_Free(schema_config.char_table);
	StringBuf_Free(schema_config.charlog_table);
	StringBuf_Free(schema_config.bonus_script_table);
	StringBuf_Free(schema_config.cart_table);
	StringBuf_Free(schema_config.inventory_table);
	StringBuf_Free(schema_config.memo_table);
	StringBuf_Free(schema_config.scdata_table);
	StringBuf_Free(schema_config.skill_table);
	StringBuf_Free(schema_config.skillcooldown_table);
	StringBuf_Free(schema_config.storage_table);
	StringBuf_Free(schema_config.friend_table);
	StringBuf_Free(schema_config.hotkey_table);
	StringBuf_Free(schema_config.mail_table);
	StringBuf_Free(schema_config.quest_table);
	StringBuf_Free(schema_config.pet_table);
	StringBuf_Free(schema_config.elemental_table);
	StringBuf_Free(schema_config.party_table);

	StringBuf_Free(schema_config.homunculus_table);
	StringBuf_Free(schema_config.homunculus_skill_table);

	StringBuf_Free(schema_config.mercenary_table);
	StringBuf_Free(schema_config.mercenary_owner_table);

	StringBuf_Free(schema_config.guild_table);
	StringBuf_Free(schema_config.guild_alliance_table);
	StringBuf_Free(schema_config.guild_castle_table);
	StringBuf_Free(schema_config.guild_expulsion_table);
	StringBuf_Free(schema_config.guild_member_table);
	StringBuf_Free(schema_config.guild_position_table);
	StringBuf_Free(schema_config.guild_skill_table);
	StringBuf_Free(schema_config.guild_storage_table);

	StringBuf_Free(schema_config.acc_reg_num_table);
	StringBuf_Free(schema_config.acc_reg_str_table);
	StringBuf_Free(schema_config.auction_table);
	StringBuf_Free(schema_config.char_reg_num_table);
	StringBuf_Free(schema_config.char_reg_str_table);
	StringBuf_Free(schema_config.ragsrvinfo_table);
	StringBuf_Free(schema_config.interlog_table);
}

/*
 * Message conf function
 */
int char_msg_config_read(char *cfgName){
	return _msg_config_read(cfgName,CHAR_MAX_MSG,msg_table);
}
const char* char_msg_txt(int msg_number){
	return _msg_txt(msg_number,CHAR_MAX_MSG,msg_table);
}
void char_do_final_msg(void){
	_do_final_msg(CHAR_MAX_MSG,msg_table);
}


void do_final(void)
{
	ShowStatus("Terminating...\n");

	char_set_all_offline(-1);
	char_set_all_offline_sql();

	inter_final();

	flush_fifos();

	do_final_msg();
	do_final_chmapif();
	do_final_chlogif();

	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s`", charserv_table(ragsrvinfo_table)) )
		Sql_ShowDebug(sql_handle);

	char_db_->destroy(char_db_, NULL);
	online_char_db->destroy(online_char_db, NULL);
	auth_db->destroy(auth_db, NULL);
	
	char_schema_config_final();

	if( char_fd != -1 )
	{
		do_close(char_fd);
		char_fd = -1;
	}

	Sql_Free(sql_handle);
	mapindex_final();

	ShowStatus("Finished.\n");
}


void set_server_type(void){
	SERVER_TYPE = ATHENA_SERVER_CHAR;
}

//------------------------------
// Function called when the server
// has received a crash signal.
//------------------------------
void do_abort(void)
{
}

/// Called when a terminate signal is received.
void do_shutdown(void) {
	if( runflag != CHARSERVER_ST_SHUTDOWN )
	{
		int id;
		runflag = CHARSERVER_ST_SHUTDOWN;
		ShowStatus("Shutting down...\n");
		// TODO proper shutdown procedure; wait for acks?, kick all characters, ... [FlavoJS]
		for( id = 0; id < ARRAYLENGTH(map_server); ++id )
			chmapif_server_reset(id);
		chlogif_check_shutdown();
		flush_fifos();
		runflag = CORE_ST_STOP;
	}
}


int do_init(int argc, char **argv)
{
	//Read map indexes
	runflag = CHARSERVER_ST_STARTING;
	mapindex_init();

	// Init default value
	CHAR_CONF_NAME =   "conf/char_athena.conf";
	LAN_CONF_NAME =    "conf/subnet_athena.conf";
	SQL_CONF_NAME =    "conf/inter_athena.conf";
	MSG_CONF_NAME_EN = "conf/msg_conf/char_msg.conf";
	safestrncpy(console_log_filepath, "./log/char-msg_log.log", sizeof(console_log_filepath));

	cli_get_options(argc,argv);

	char_set_defaults();
	char_schema_config_init();
	char_config_read(CHAR_CONF_NAME, true);
	char_lan_config_read(LAN_CONF_NAME);
	msg_config_read(MSG_CONF_NAME_EN);

	if (strcmp(charserv_config.userid, "s1")==0 && strcmp(charserv_config.passwd, "p1")==0) {
		ShowWarning("Using the default user/password s1/p1 is NOT RECOMMENDED.\n");
		ShowNotice("Please edit your 'login' table to create a proper inter-server user/password (gender 'S')\n");
		ShowNotice("And then change the user/password to use in conf/char_athena.conf (or conf/import/char_conf.txt)\n");
	}

	inter_init_sql((argc > 2) ? argv[2] : inter_cfgName); // inter server configuration

	auth_db = idb_alloc(DB_OPT_RELEASE_DATA);
	online_char_db = idb_alloc(DB_OPT_RELEASE_DATA);
	char_mmo_sql_init();
	char_read_fame_list(); //Read fame lists.

	if ((naddr_ != 0) && (!(charserv_config.login_ip) || !(charserv_config.char_ip) ))
	{
		char ip_str[16];
		ip2str(addr_[0], ip_str);

		if (naddr_ > 1)
			ShowStatus("Multiple interfaces detected..  using %s as our IP address\n", ip_str);
		else
			ShowStatus("Defaulting to %s as our IP address\n", ip_str);
		if (!(charserv_config.login_ip) ) {
			safestrncpy(charserv_config.login_ip_str, ip_str, sizeof(charserv_config.login_ip_str));
			charserv_config.login_ip = str2ip(charserv_config.login_ip_str);
		}
		if (!(charserv_config.char_ip)) {
			safestrncpy(charserv_config.char_ip_str, ip_str, sizeof(charserv_config.char_ip_str));
			charserv_config.char_ip = str2ip(charserv_config.char_ip_str);
		}
	}

	do_init_chlogif();
	do_init_chmapif();

	// periodically update the overall user count on all mapservers + login server
	add_timer_func_list(chlogif_broadcast_user_count, "broadcast_user_count");
	add_timer_interval(gettick() + 1000, chlogif_broadcast_user_count, 0, 0, 5 * 1000);

	// Timer to clear (online_char_db)
	add_timer_func_list(char_chardb_waiting_disconnect, "chardb_waiting_disconnect");

	// Online Data timers (checking if char still connected)
	add_timer_func_list(char_online_data_cleanup, "online_data_cleanup");
	add_timer_interval(gettick() + 1000, char_online_data_cleanup, 0, 0, 600 * 1000);

	//check db tables
	if(charserv_config.char_check_db && char_checkdb() == 0){
		ShowFatalError("char : A table is missing in sql-server, please fix it, see (sql-files/main.sql for structure) \n");
		exit(EXIT_FAILURE);
	}
	//Cleaning the tables for NULL entrys @ startup [Sirius]
	//Chardb clean
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `account_id` = '0'", charserv_table(char_table)) )
		Sql_ShowDebug(sql_handle);

	//guilddb clean
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `guild_lv` = '0' AND `max_member` = '0' AND `exp` = '0' AND `next_exp` = '0' AND `average_lv` = '0'", charserv_table(guild_table)) )
		Sql_ShowDebug(sql_handle);

	//guildmemberdb clean
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `guild_id` = '0' AND `account_id` = '0' AND `char_id` = '0'", charserv_table(guild_member_table)) )
		Sql_ShowDebug(sql_handle);

	set_defaultparse(chclif_parse);

	if( (char_fd = make_listen_bind(charserv_config.bind_ip,charserv_config.char_port)) == -1 ) {
		ShowFatalError("Failed to bind to port '"CL_WHITE"%d"CL_RESET"'\n",charserv_config.char_port);
		exit(EXIT_FAILURE);
	}

	if( runflag != CORE_ST_STOP )
	{
		shutdown_callback = do_shutdown;
		runflag = CHARSERVER_ST_RUNNING;
	}

	do_init_chcnslif();
	mapindex_check_mapdefault(charserv_config.default_map);
	ShowInfo("Default map: '"CL_WHITE"%s %d,%d"CL_RESET"'\n", charserv_config.default_map, charserv_config.default_map_x, charserv_config.default_map_y);

	ShowStatus("The char-server is "CL_GREEN"ready"CL_RESET" (Server is listening on the port %d).\n\n", charserv_config.char_port);

	return 0;
}