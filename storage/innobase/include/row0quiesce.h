/*****************************************************************************

Copyright (c) 2012, 2020, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is also distributed with certain software (including but not
limited to OpenSSL) that is licensed under separate terms, as designated in a
particular file or component or in included license documentation. The authors
of MySQL hereby grant you an additional permission to link the program and
your derivative works with the separately licensed software that they have
included with MySQL.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file include/row0quiesce.h

 Header file for tablespace quiesce functions.

 Created 2012-02-08 by Sunny Bains
 *******************************************************/

#ifndef row0quiesce_h
#define row0quiesce_h

#include "dict0types.h"
#include "univ.i"

struct trx_t;

/** The version number of the export meta-data text file. */
#define IB_EXPORT_CFG_VERSION_V1 1
/** The v2 .cfg has space flags written */
#define IB_EXPORT_CFG_VERSION_V2 2
/** The v3 .cfg writes instant column default values in metadata section. */
#define IB_EXPORT_CFG_VERSION_V3 0x3UL
/** The v4 .cfg has the is_ascending boolean written for each index column. */
#define IB_EXPORT_CFG_VERSION_V4 4
/** The v5 .cfg writes number of nullable column in table before first instant
 * column. */
#define IB_EXPORT_CFG_VERSION_V5 5
/** The v6 .cfg writes the Compression::Type of the table. */
#define IB_EXPORT_CFG_VERSION_V6 6
/** Future version used to test that the correct error message is returned. */
#define IB_EXPORT_CFG_VERSION_V99 99
#define IB_EXPORT_CFG_VERSION_V1_WITH_RK 0xFFFFFFFF

/** Quiesce the tablespace that the table resides in.
@param[in] table Quiesce this table
@param[in,out] trx Transaction/session */
void row_quiesce_table_start(dict_table_t *table, trx_t *trx);

/** Set a table's quiesce state.
 @return DB_SUCCESS or errro code. */
dberr_t row_quiesce_set_state(
    dict_table_t *table, /*!< in: quiesce this table */
    ib_quiesce_t state,  /*!< in: quiesce state to set */
    trx_t *trx)          /*!< in/out: transaction */
    MY_ATTRIBUTE((warn_unused_result));

/** Cleanup after table quiesce.
@param[in] table Quiesce this table
@param[in,out] trx Transaction/session */
void row_quiesce_table_complete(dict_table_t *table, trx_t *trx);

#endif /* row0quiesce_h */
