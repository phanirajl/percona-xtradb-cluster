# InnoDB transparent tablespace data encryption
# This test case will test basic encryption support features.

# Restart the server with keyring loaded
--let $restart_parameters = $KEYRING2_RESTART_PARAM
--let $restart_hide_args = 1
--source include/restart_mysqld.inc

let $innodb_file_per_table = `SELECT @@innodb_file_per_table`;

SET GLOBAL innodb_file_per_table = 1;
SELECT @@innodb_file_per_table;

# Create a table with encryption
eval CREATE TABLE t1(c1 INT, c2 char(20)) ENCRYPTION="$encryption_type" ENGINE = InnoDB;

SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES(0, "aaaaa");
INSERT INTO t1 VALUES(1, "bbbbb");
INSERT INTO t1 VALUES(2, "ccccc");
INSERT INTO t1 VALUES(3, "ddddd");
INSERT INTO t1 VALUES(4, "eeeee");
INSERT INTO t1 VALUES(5, "fffff");
INSERT INTO t1 VALUES(6, "ggggg");
INSERT INTO t1 VALUES(7, "hhhhh");
INSERT INTO t1 VALUES(8, "iiiii");
INSERT INTO t1 VALUES(9, "jjjjj");
INSERT INTO t1 select * from t1;
INSERT INTO t1 select * from t1;
INSERT INTO t1 select * from t1;
INSERT INTO t1 select * from t1;
INSERT INTO t1 select * from t1;
INSERT INTO t1 select * from t1;

SELECT * FROM t1 LIMIT 10;

# Restart to confirm the encryption info can be retrieved properly.
--let $restart_parameters = $keyring2_restart_param
--let $restart_hide_args = 1
--source include/restart_mysqld.inc

SELECT * FROM t1 LIMIT 10;

# Key rotation.
ALTER INSTANCE ROTATE INNODB MASTER KEY;

DROP TABLE t1;

# Crash/recovery test.
eval CREATE TABLE t1(c1 INT, c2 char(20)) ENCRYPTION="$encryption_type" ENGINE = InnoDB;

INSERT INTO t1 VALUES(0, "aaaaa");
INSERT INTO t1 VALUES(1, "bbbbb");
INSERT INTO t1 VALUES(2, "ccccc");
INSERT INTO t1 VALUES(3, "ddddd");
INSERT INTO t1 VALUES(4, "eeeee");
INSERT INTO t1 VALUES(5, "fffff");
INSERT INTO t1 VALUES(6, "ggggg");
INSERT INTO t1 VALUES(7, "hhhhh");
INSERT INTO t1 VALUES(8, "iiiii");
INSERT INTO t1 VALUES(9, "jjjjj");

# Restart to confirm the encryption info can be retrieved properly.
--let $restart_parameters = $keyring2_restart_param
--let $restart_hide_args = 1
--source include/restart_mysqld.inc

SELECT * FROM t1 LIMIT 10;
DROP TABLE t1;

--let $restart_parameters = $keyring1_restart_param --general-log --log-output=FILE --general_log_file=$MYSQL_TMP_DIR/keyring_query_log
--let $restart_hide_args = 1
--source include/restart_mysqld.inc

# Check no effect of block_encryption_mode = 'aes-256-cbc' variable on table encryption
SET block_encryption_mode = 'aes-256-cbc';
# Test encryption .
--disable_warnings
DROP DATABASE IF EXISTS tde_db;
CREATE DATABASE tde_db;
eval CREATE TABLE tde_db.t1(c1 INT PRIMARY KEY, c2 char(50)) ENCRYPTION = '$encryption_type' ENGINE = InnoDB;
--enable_warnings
#
INSERT INTO tde_db.t1 VALUES(0, 'abc');
INSERT INTO tde_db.t1 VALUES(1, 'xyz');
INSERT INTO tde_db.t1 VALUES(2, null);
INSERT INTO tde_db.t1 VALUES(3, null);
SELECT * FROM tde_db.t1 LIMIT 10;
ALTER INSTANCE ROTATE INNODB MASTER KEY;
SELECT * FROM tde_db.t1 LIMIT 10;
--echo # Mysqldump output
--exec $MYSQL_DUMP --compact --skip-comments --databases tde_db
--echo # Redirecting mysqlpump output to MYSQL_TMP_DIR/mysqlpump_encrypt.sql
--exec $MYSQL_PUMP --default-parallelism=1 --databases tde_db > $MYSQL_TMP_DIR/mysqlpump_encrypt.sql
DROP DATABASE tde_db;

--let SEARCH_FILE=$MYSQL_TMP_DIR/keyring_query_log
let SEARCH_PATTERN= ALTER INSTANCE ROTATE INNODB MASTER KEY;
--source include/search_pattern.inc

--echo # Loading tables from mysqlpump_encrypt.sql
--exec $MYSQL --skip-comments < $MYSQL_TMP_DIR/mysqlpump_encrypt.sql
SELECT * FROM tde_db.t1 LIMIT 10;
INSERT INTO tde_db.t1 VALUES(4, null);
SELECT * FROM tde_db.t1 LIMIT 10;
DROP DATABASE tde_db;
#


--echo #
--echo # Bug #26634507  CREATE_OPTIONS FLD IN INFORMATION_SCHEMA.TABLES NOT
--echo # FILLING PROPERLY.
--echo # The CREATE_OPTIONS field from I_S.TABLES should show the option
--echo # 'ENCRYPTION='.
--echo #

CREATE TABLE not_encrypted1 (col1 INT) ENCRYPTION='n';
CREATE TABLE not_encrypted2 (col1 INT) ENCRYPTION='N';
CREATE TABLE encrypted1 (col1 INT) ENCRYPTION='y';
CREATE TABLE encrypted2 (col1 INT) ENCRYPTION='Y';

SELECT TABLE_SCHEMA, TABLE_NAME, CREATE_OPTIONS
  FROM INFORMATION_SCHEMA.TABLES
  WHERE TABLE_NAME like '%encrypted%'
  ORDER BY TABLE_NAME;

DROP TABLE encrypted1;
DROP TABLE not_encrypted1;
DROP TABLE encrypted2;
DROP TABLE not_encrypted2;



# Cleanup

--let $restart_parameters = $keyring2_restart_param
--let $restart_hide_args = 1
--source include/restart_mysqld.inc

--remove_file $MYSQL_TMP_DIR/keyring_query_log
--remove_file $MYSQL_TMP_DIR/mysqlpump_encrypt.sql
eval SET GLOBAL innodb_file_per_table=$innodb_file_per_table;
