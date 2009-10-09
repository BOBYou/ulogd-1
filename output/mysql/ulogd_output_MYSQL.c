/* ulogd_MYSQL.c, Version $Revision$
 *
 * ulogd output plugin for logging to a MySQL database
 *
 * (C) 2000-2005 by Harald Welte <laforge@gnumonks.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 
 *  as published by the Free Software Foundation
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 * 
 * $Id$
 *
 * 15 May 2001, Alex Janssen <alex@ynfonatic.de>:
 *      Added a compability option for older MySQL-servers, which
 *      don't support mysql_real_escape_string
 *
 * 17 May 2001, Alex Janssen <alex@ynfonatic.de>:
 *      Added the --with-mysql-log-ip-as-string feature. This will log
 *      IP's as string rather than an unsigned long integer to the database.
 *	See ulogd/doc/mysql.table.ipaddr-as-string as an example.
 *	BE WARNED: This has _WAY_ less performance during table searches.
 *
 * 09 Feb 2005, Sven Schuster <schuster.sven@gmx.de>:
 * 	Added the "port" parameter to specify ports different from 3306
 *
 * 12 May 2005, Jozsef Kadlecsik <kadlec@blackhole.kfki.hu>
 *	Added reconnecting to lost mysql server.
 *
 * 15 Oct 2005, Harald Welte <laforge@netfilter.org>
 * 	Port to ulogd2 (@ 0sec conference, Bern, Suisse)
 */
#include "config.h"
#include <ulogd/ulogd.h>
#include <ulogd/common.h>
#include <ulogd/plugin.h>
#include <ulogd/db.h>
#include <time.h>
#include <arpa/inet.h>
#include <mysql/mysql.h>

#ifdef DEBUG_MYSQL
#define DEBUGP(x, args...)	fprintf(stderr, x, ## args)
#else
#define DEBUGP(x, args...)
#endif

struct mysql_instance {
	struct db_instance db_inst;
	MYSQL *dbh; /* the database handle we are using */
};

/* our configuration directives */
static const struct config_keyset kset_mysql = {
	.num_ces = DB_CE_NUM+5,
	.ces = {
		DB_CES,
		{
			.key = "db", 
			.type = CONFIG_TYPE_STRING,
			.options = CONFIG_OPT_MANDATORY,
		},
		{
			.key = "host", 
			.type = CONFIG_TYPE_STRING,
			.options = CONFIG_OPT_MANDATORY,
		},
		{
			.key = "user", 
			.type = CONFIG_TYPE_STRING,
			.options = CONFIG_OPT_MANDATORY,
		},
		{
			.key = "pass", 
			.type = CONFIG_TYPE_STRING,
			.options = CONFIG_OPT_MANDATORY,
		},
		{
			.key = "port",
			.type = CONFIG_TYPE_INT,
		},
	},
};

#define db_ce(pi)	ulogd_config_str((pi), DB_CE_NUM)
#define	host_ce(pi)	ulogd_config_str((pi), DB_CE_NUM + 1)
#define user_ce(pi)	ulogd_config_str((pi), DB_CE_NUM + 2)
#define pass_ce(pi)	ulogd_config_str((pi), DB_CE_NUM + 3)
#define port_ce(pi)	ulogd_config_int((pi), DB_CE_NUM + 4)

/* find out which columns the table has */
static int get_columns_mysql(struct ulogd_pluginstance *upi)
{
	struct mysql_instance *mi = upi_priv(upi);
	MYSQL_RES *result;
	MYSQL_FIELD *field;
	struct ulogd_key *f, *f2;
	int i;

	if (!mi->dbh) {
		ulogd_log(ULOGD_ERROR, "no database handle\n");
		return -1;
	}

	result = mysql_list_fields(mi->dbh, table_ce(upi), NULL);
	if (!result) {
		ulogd_log(ULOGD_ERROR, "error in list_fields(): %s\n",
			  mysql_error(mi->dbh));
		return -1;
	}

	/* Thea idea here is that we can create a pluginstance specific input
	 * key array by not specifyling a plugin input key list.  ulogd core
	 * will then set upi->input to NULL.  Yes, this creates a memory hole
	 * in case the core just calls ->configure() and then aborts (and thus
	 * never free()s the memory we allocate here.  FIXME. */

	upi->input.num_keys = mysql_num_fields(result);
	ulogd_log(ULOGD_DEBUG, "%u fields in table\n", upi->input.num_keys);
	upi->input.keys = malloc(sizeof(struct ulogd_key) * 
						upi->input.num_keys);
	if (!upi->input.keys) {
		upi->input.num_keys = 0;
		ulogd_log(ULOGD_ERROR, "ENOMEM\n");
		return -ENOMEM;
	}
	
	memset(upi->input.keys, 0, sizeof(struct ulogd_key) *
						upi->input.num_keys);

	for (i = 0; field = mysql_fetch_field(result); i++) {
		char buf[ULOGD_MAX_KEYLEN+1];
		char *underscore;
		int id;

		/* replace all underscores with dots */
		strncpy(buf, field->name, ULOGD_MAX_KEYLEN);
		while ((underscore = strchr(buf, '_')))
			*underscore = '.';

		DEBUGP("field '%s' found\n", buf);

		/* add it to list of input keys */
		strncpy(upi->input.keys[i].name, buf, ULOGD_MAX_KEYLEN);
	}
	/* MySQL Auto increment ... ID :) */
	upi->input.keys[0].flags |= ULOGD_KEYF_INACTIVE;
	
	mysql_free_result(result);
	return 0;
}

static int close_db_mysql(struct ulogd_pluginstance *upi)
{
	struct mysql_instance *mi = upi_priv(upi);
	mysql_close(mi->dbh);
	return 0;
}

/* make connection and select database */
static int open_db_mysql(struct ulogd_pluginstance *upi)
{
	struct mysql_instance *mi = upi_priv(upi);
	unsigned connect_timeout = timeout_ce(upi);
	char *server = host_ce(upi);
	u_int16_t port = port_ce(upi);
	char *user = user_ce(upi);
	char *pass = pass_ce(upi);
	char *db = db_ce(upi);

	mi->dbh = mysql_init(NULL);
	if (!mi->dbh) {
		ulogd_log(ULOGD_ERROR, "error in mysql_init()\n");
		return -1;
	}

	if (connect_timeout)
		mysql_options(mi->dbh, MYSQL_OPT_CONNECT_TIMEOUT, 
			      (const char *) &connect_timeout);

	if (!mysql_real_connect(mi->dbh, server, user, pass, db, port, NULL, 0)) {
		ulogd_log(ULOGD_ERROR, "can't connect to db: %s\n",
			  mysql_error(mi->dbh));
		return -1;
	}
		
	return 0;
}

static int execute_mysql(struct ulogd_pluginstance *upi,
			 const char *stmt, unsigned int len)
{
	struct mysql_instance *mi = upi_priv(upi);
	int ret;

	ret = mysql_real_query(mi->dbh, stmt, len);
	if (ret) {
		ulogd_log(ULOGD_ERROR, "execute failed (%s)\n",
			  mysql_error(mi->dbh));
		return -1;
	}

	return 0;
}

static struct db_driver db_driver_mysql = {
	.get_columns	= &get_columns_mysql,
	.open_db	= &open_db_mysql,
	.close_db	= &close_db_mysql,
	.execute	= &execute_mysql,
};

static int configure_mysql(struct ulogd_pluginstance *upi)
{
	struct db_instance *di = upi_priv(upi);

	di->driver = &db_driver_mysql;

	return ulogd_db_configure(upi);
}

static struct ulogd_plugin plugin_mysql = {
	.name = "MYSQL",
	.input = {
		.keys = NULL,
		.num_keys = 0,
		.type = ULOGD_DTYPE_PACKET | ULOGD_DTYPE_FLOW, 
	},
	.output = {
		.type = ULOGD_DTYPE_SINK,
	},
	.config_kset = &kset_mysql,
	.priv_size = sizeof(struct mysql_instance),
	.configure = &configure_mysql,
	.start	   = &ulogd_db_start,
	.stop	   = &ulogd_db_stop,
	.signal	   = &ulogd_db_signal,
	.interp	   = &ulogd_db_interp,
	.rev		 = ULOGD_PLUGIN_REVISION,
};

void __upi_ctor init(void);

void init(void) 
{
	ulogd_register_plugin(&plugin_mysql);
}
