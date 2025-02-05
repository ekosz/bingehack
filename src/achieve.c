#include <stdlib.h>
#include <stdbool.h>
#include <dlfcn.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>

#include <mysql.h>
#include <libconfig.h>

#include "hack.h"
#include "configfile.h"
#include "achieve.h"
#include "mysql_library.h"

static MYSQL db;

const int ACHIEVEMENT_DEBUG = 0; 
int achievement_system_disabled = 0;
static bool achievement_system_initialized = false;

bool achievement_system_startup() {
	if( achievement_system_initialized ) return true;
	if( !configfile_available() || !mysql_library_available() ) {
		disable_achievements();
		return true;
	}

	//read database configuration from file
	const char *db_user, *db_pass, *db_db, *db_server;
	if( !configfile_get_string("achievements.mysql.username", &db_user) ||
	    !configfile_get_string("achievements.mysql.password", &db_pass) ||
	    !configfile_get_string("achievements.mysql.database", &db_db) ||
	    !configfile_get_string("achievements.mysql.server",   &db_server) ) return false;

	mysql.init(&db);

	//Respond in 3 seconds or assume db server is unreachable
	int timeout_seconds = 3;
	int ret = mysql.options(&db, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_seconds);
	assert(ret == 0);

	if(mysql.real_connect(&db, db_server, db_user, db_pass, db_db, 0, NULL, 0) == NULL){
		pline("Can't connect to achievements database. Database error: %s", mysql.error(&db));
		mysql.close(&db);
		disable_achievements();
		return false;
	}

	achievement_system_initialized = true;
	return true;
}

void achievement_system_shutdown() {
	if( achievement_system_initialized ) mysql.close(&db);
}

// Convenience function for awarding achievements that have no progress metric
int award_achievement(int achievement_id){
	return add_achievement_progress(achievement_id, get_achievement_max_progress(achievement_id));
}

// Return 1 on success, 0 on failure
int add_achievement_progress(int achievement_id, int add_progress_count){
	if(achievement_system_disabled){ return ACHIEVEMENT_PUSH_FAILURE; }
	if(ACHIEVEMENT_DEBUG){pline("DEBUG: add_achievement_progress(%i, %i)", achievement_id, add_progress_count);}
	
	if(check_db_connection()){
		disable_achievements();
	}
	
	//Check if user exists
	if(!user_exists()){
		register_user();
	}
	//Calculate user's completion on this achievement
	int pre_achievement_progress = get_achievement_progress(achievement_id);
	int max_achievement_progress = get_achievement_max_progress(achievement_id);
	if(ACHIEVEMENT_DEBUG){pline("DEBUG: get_achievement_max_progress(%i)=%i", achievement_id, max_achievement_progress);}

	if(pre_achievement_progress < max_achievement_progress){ //user still needs achievement
		if(pre_achievement_progress + add_progress_count >= max_achievement_progress){ //Achievement fully achieved!
			if(push_achievement_progress(achievement_id, max_achievement_progress)){ //floor the value to max_progress
				char * achievement_name = get_achievement_name(achievement_id);
				pline("Congratulations! You've earned the achievement: %s", achievement_name);
				free(achievement_name);
				return ACHIEVEMENT_PUSH_SUCCESS;
			}
			else{
				pline("Er, oops. You got an achievement, but it can't be recorded.");
				return ACHIEVEMENT_PUSH_FAILURE;
			}
		}
		else{ //user stills needs achievement, but isn't quite there yet
			if(push_achievement_progress(achievement_id, pre_achievement_progress+add_progress_count)){
				return ACHIEVEMENT_PUSH_SUCCESS;
			}
			else{
				return ACHIEVEMENT_PUSH_FAILURE;
			}
		}
	}
	return ACHIEVEMENT_PUSH_SUCCESS;
}

int get_achievement_progress(int achievement_id){
	if( achievement_system_disabled ) return -1;

	char* query;
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	char* str_progress;
	int achievement_progress = -1;

	char *name = mysql_library_escape_string(&db, plname);
	if( asprintf(&query, "SELECT `achievement_progress`.`progress` FROM `achievement_progress` JOIN `users_in_apps` ON `users_in_apps`.`user_id` = `achievement_progress`.`user_id` where app_username = '%s' and app_id = %i and achievement_id = %i;", name, GAME_ID, achievement_id) == -1 ) panic("asprintf: %s", strerror(errno));
	free(name);
	if( mysql.real_query(&db, query, (unsigned int) strlen(query)) != 0 ) goto fail;
	free(query);
	if( (res = mysql.use_result(&db)) == NULL ) goto fail;
	if( (row = mysql.fetch_row(res)) == NULL ) {
		if( mysql.num_rows(res) == 0 ) {
			achievement_progress = 0;
			goto out;
		} else {
			goto fail;
		}
	} else {
		if( asprintf(&str_progress, "%s", row[0]) == -1 ) panic("asprintf: %s", strerror(errno));
		achievement_progress = atoi(str_progress);
		free(str_progress);
	}

	if(ACHIEVEMENT_DEBUG) pline("DEBUG: get_achievement_progress(%i)=%i", achievement_id, achievement_progress);
	
	row = mysql.fetch_row(res);
	assert(row == NULL);
	goto out;

fail:
	pline("Error in get_achievement_progress(%i) (Error: %s)", achievement_id, mysql.error(&db));
	disable_achievements();
	achievement_progress = -1;

out:
	if( res != NULL ) mysql.free_result(res);
	return achievement_progress;
}

int get_achievement_max_progress(int achievement_id){
	if( achievement_system_disabled ) return -1;

	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	char* query;
	char* str_max_progress;
	int max_progress=0;

	if( asprintf(&query, "SELECT `achievements`.`progress_max` FROM `achievements` WHERE id = %i ;", achievement_id) == -1 ) panic("asprintf: %s", strerror(errno));
	if( mysql.real_query(&db, query, (unsigned int) strlen(query)) != 0 ) goto fail;
	free(query);
	if( (res = mysql.use_result(&db)) == NULL ) goto fail;

	if( (row = mysql.fetch_row(res)) == NULL ) {
		if( mysql.num_rows(res) == 0 ) panic("Error in get_achievement_max_progress(%i): Requested achievement does not exist.", achievement_id);
		goto fail;
	}
	if( asprintf(&str_max_progress, "%s", row[0]) == -1 ) panic("asprintf: %s", strerror(errno));
	max_progress = atoi(str_max_progress);

	free(str_max_progress);

	row = mysql.fetch_row(res);
	assert(row == NULL);
	goto out;

fail:
	pline("Error in get_achievement_max_progress(%i) (Error: %s)", achievement_id, mysql.error(&db));
	disable_achievements();
	max_progress = -1;

out:
	if( res != NULL ) mysql.free_result(res);
	return max_progress;
}

//REQUIRES timeout on non-success!!!!!!!
int push_achievement_progress(int achievement_id, int updated_progress_count){
	char* query;

	int progress = get_achievement_progress(achievement_id);
	if( progress == -1 ) return -1;
	if( progress == 0) { //no previous progress on achievement, INSERT time
		char *name = mysql_library_escape_string(&db, plname);
		if( asprintf(&query, "INSERT INTO `achievement_progress`(`user_id`, `achievement_id`, `progress`) VALUES ((select user_id from `users_in_apps` where app_id=1 and app_username='%s'), %i, %i);", name, achievement_id, updated_progress_count) == -1 ) panic("asprintf: %s", strerror(errno));
		free(name);
		if(mysql.real_query(&db, query, (unsigned int) strlen(query)) != 0){
			pline("Real_query failed in push_achievement_progress: %s", mysql.error(&db));
			disable_achievements();
		}
		free(query);
		return 1;
	} else { //TODO: Implement Multi-part achievements (aka UPDATE)
		return 0;
	}
}

// It is the caller's responsibility to free() the return value of this function.
char *get_achievement_name( int achievement_id ){
		MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	char* query;
	char* str_name;

	if( asprintf(&query, "SELECT `achievements`.`title` FROM `achievements` WHERE id = %i ;", achievement_id) == -1 ) panic("asprintf: %s", strerror(errno));
	if( mysql.real_query(&db, query, (unsigned int) strlen(query)) != 0 ) goto fail;
	free(query);

	if( (res = mysql.use_result(&db)) == NULL ) goto fail;
	if( (row = mysql.fetch_row(res)) == NULL ) {
		if( mysql.num_rows(res) == 0 ) panic("Attempt to call non-existant achievement_id %i", achievement_id);
		goto fail;
	}
	if( (str_name = strdup(row[0])) == NULL ) panic("strdup: %s", strerror(errno));
	if(ACHIEVEMENT_DEBUG) pline("DEBUG: get_achievements_name(%i)='%s'", achievement_id, str_name);

	row = mysql.fetch_row(res);
	assert(row == NULL);

	goto out;

fail:
	pline("Error in get_achievement_name(%i) (Error: %s)", achievement_id, mysql.error(&db));
	disable_achievements();
	str_name = strdup("");
	if( str_name == NULL ) panic("strdup: %s", strerror(errno));

out:
	if( res != NULL ) mysql.free_result(res);
	return str_name;
}

void disable_achievements(){
	pline("Disabling the achievements system for the duration of this session.");
	achievement_system_disabled = 1;
}

//TODO Odd unregistered case where app_username is already taken
int register_user(){
	pline("Hey! Listen! You're not registered in the achievements database.");
	pline("If you don't have a CSH account, you can probably leave this blank.");
	char str[BUFSZ];
	getlin("Enter CSH username to associate with this account:", str);
	if(isspace(str[0]) || str[0] == '\033' || str[0] == '\n' || str[0] == 0){
		memset(str, 0, BUFSZ);
		strcpy(str,plname);
	}
	
	char *query;
	if( asprintf(&query, "INSERT INTO `users` (`username`) VALUES ('%s')", str ) == -1) panic("asprintf: %s", strerror(errno));
	
	if(mysql.real_query(&db, query, (unsigned int) strlen(query)) != 0){
		pline("Real_query failed in register_user: %s", mysql.error(&db));
		disable_achievements();
	}
	free(query);
	
	char *allocstr = mysql_library_escape_string(&db, str),
		 *name = mysql_library_escape_string(&db, plname);
	if( asprintf(&query, "INSERT INTO `users_in_apps` VALUES ((SELECT `id` FROM `users` WHERE `users`.`username` = '%s'), %i, '%s')", allocstr, GAME_ID, name) == -1) panic("asprintf: %s", strerror(errno));
	free(allocstr);
	free(name);

	if(mysql.real_query(&db, query, (unsigned int) strlen(query)) != 0){
		pline("Real_query failed in register_user: %s", mysql.error(&db));
		disable_achievements();
	}
	free(query);

	return 1;
}

int user_exists(){
	MYSQL_RES *res = NULL;
	MYSQL_ROW row;
	char* query;
	int user_exists = 0;

	char *name = mysql_library_escape_string(&db, plname);
	if( asprintf(&query, "SELECT `app_username` FROM `users_in_apps` WHERE app_username = '%s' AND app_id = %i ;", name, GAME_ID) == -1 ) panic("asprintf: %s", strerror(errno));
	free(name);
	if( mysql.real_query(&db, query, (unsigned int) strlen(query)) != 0 ){
		goto fail;
	}
	free(query);

	if((res = mysql.use_result(&db)) == NULL){
		disable_achievements();
	}
	else{
		if( (row = mysql.fetch_row(res)) == NULL ) {
			if( mysql.num_rows(res) == 0 ){ //Not a user of nethack
				user_exists = 0;
			}
		}
		else{
			row = mysql.fetch_row(res);
			assert(row == NULL);
			user_exists = 1;
		}
	}
	goto out;

fail:
	pline("Error in user_exists() (Error: %s)", mysql.error(&db));
	disable_achievements();
out:
	if( res != NULL ) mysql.free_result(res);

	return user_exists;
}


int check_db_connection(){
	if(mysql.ping(&db)){ // what we have here is a failure to communicate
		pline("Error while attempting to reconnect to DB: %s", mysql.error(&db));
		return 1;
	}
	else{
		return 0;
	}
}
