/***********************************************************************

Copyright (c) 2019, 2020, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License, version 2.0,
as published by the Free Software Foundation.

This program is also distributed with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have included with MySQL.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License, version 2.0, for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

***********************************************************************/

/** @file os/os0enc.cc
 Encryption code. */

#include "os0enc.h"
#include "fil0crypt.h"
#include "fil0fil.h"
#ifdef UNIV_HOTBACKUP
#include "fsp0file.h"
#endif /* UNIV_HOTBACKUP */
#include "log0log.h"
#include "mach0data.h"
#include "os0file.h"
#include "page0page.h"
#include "system_key.h"
#include "ut0crc32.h"

#include <errno.h>
#include "my_aes.h"
#include "my_rnd.h"
#include "mysql/service_mysql_keyring.h"
#include "mysqld.h"

constexpr char Encryption::KEY_MAGIC_V1[];
constexpr char Encryption::KEY_MAGIC_V2[];
constexpr char Encryption::KEY_MAGIC_V3[];
constexpr char Encryption::KEY_MAGIC_RK_V1[];
constexpr char Encryption::KEY_MAGIC_RK_V2[];
constexpr char Encryption::KEY_MAGIC_PS_V1[];
constexpr char Encryption::KEY_MAGIC_PS_V2[];
constexpr char Encryption::KEY_MAGIC_PS_V3[];

constexpr char Encryption::MASTER_KEY_PREFIX[];
constexpr char Encryption::DEFAULT_MASTER_KEY[];

constexpr char Encryption::ZIP_PAGE_KEYRING_ENCRYPTION_MAGIC[];
constexpr char Encryption::PERCONA_SYSTEM_KEY_PREFIX[];

/** Minimum length needed for encryption */
constexpr size_t MIN_ENCRYPTION_LEN = 2 * MY_AES_BLOCK_SIZE + FIL_PAGE_DATA;

/** Current master key id */
uint32_t Encryption::s_master_key_id = Encryption::DEFAULT_MASTER_KEY_ID;

/** Current uuid of server instance */
char Encryption::s_uuid[Encryption::SERVER_UUID_LEN + 1] = {0};

Encryption::Encryption(const Encryption &other) noexcept
    : m_type(other.m_type),
      m_key(other.m_key),
      m_klen(other.m_klen),
      m_iv(other.m_iv),
      m_tablespace_key(other.m_tablespace_key),
      m_key_version(other.m_key_version),
      m_key_id(other.m_key_id),
      m_checksum(other.m_checksum),
      m_encryption_rotation(other.m_encryption_rotation),
      m_key_versions_cache(other.m_key_versions_cache) {
  memcpy(m_key_id_uuid, other.m_key_id_uuid, SERVER_UUID_LEN + 1);
}

Encryption::~Encryption() {
}

void Encryption::set_key(byte *key, ulint key_len) noexcept {
  m_key = key;
  m_klen = key_len;
}

void Encryption::set_key_versions_cache(
    std::map<uint, byte *> *key_versions_cache) noexcept {
  m_key_versions_cache = key_versions_cache;
}

const char *Encryption::to_string(Type type) noexcept {
  switch (type) {
    case NONE:
      return ("N");
    case AES:
      return ("Y");
    case KEYRING:
      return ("KEYRING");
  }

  ut_ad(0);

  return ("<UNKNOWN>");
}

void Encryption::random_value(byte *value) noexcept {
  ut_ad(value != nullptr);

  my_rand_buffer(value, KEY_LEN);
}

void Encryption::fill_key_name(char *key_name, uint key_id, const char *uuid) {
#ifndef UNIV_INNOCHECKSUM
  // Each key that we fetch/remove/store in keyring for KEYRING encryption has
  // to go through one of fill_key_name function. All InnoDB keys used for
  // KEYRING encryption should have uuid assigned.
  ut_ad(strlen(uuid) > 0);

  memset(key_name, 0, MASTER_KEY_NAME_MAX_LEN);

  snprintf(key_name, MASTER_KEY_NAME_MAX_LEN, "%s-%u-%s",
           PERCONA_SYSTEM_KEY_PREFIX, key_id, uuid);
#endif
}

void Encryption::fill_key_name(char *key_name, uint key_id, const char *uuid,
                               uint key_version) {
#ifndef UNIV_INNOCHECKSUM
  // Each key that we fetch/remove/store in keyring for KEYRING encryption has
  // to go through one of fill_key_name function. All InnoDB keys used for
  // KEYRING encryption should have uuid assigned.
  ut_ad(strlen(uuid) > 0);

  memset(key_name, 0, MASTER_KEY_NAME_MAX_LEN);

  snprintf(key_name, MASTER_KEY_NAME_MAX_LEN, "%s-%u-%s:%u",
           PERCONA_SYSTEM_KEY_PREFIX, key_id, uuid, key_version);
#endif
}

void Encryption::create_tablespace_key(byte **tablespace_key, uint key_id,
                                       const char *uuid) {
#ifndef UNIV_INNOCHECKSUM
  char *key_type = nullptr;
  size_t key_len;
  char key_name[MASTER_KEY_NAME_MAX_LEN];
  int ret;

  // Newly created tablespace keys should always have uuid equal to server_uuid.
  // There are situations when server_uuid is not available - like when parsing
  // redo logs. Then we read uuid from crypto's redo log.
  ut_ad(strlen(server_uuid) == 0 ||
        memcmp(server_uuid, uuid, SERVER_UUID_LEN) == 0);

  fill_key_name(key_name, key_id, uuid);

  /* We call key ring API to generate tablespace key here. */
  ret = my_key_generate(key_name, "AES", nullptr, KEY_LEN);

  if (ret) {
    ib::error() << "Encryption can't generate tablespace key : " << key_name;
    *tablespace_key = nullptr;
    return;
  }

  byte *system_tablespace_key = nullptr;
  /* We call key ring API to get tablespace key here. */
  ret =
      my_key_fetch(key_name, &key_type, nullptr,
                   reinterpret_cast<void **>(&system_tablespace_key), &key_len);

  if (ret || system_tablespace_key == nullptr) {
    ib::error() << "Encryption can't find tablespace key " << key_name
                << " please check"
                   " that the keyring plugin is loaded.";
    *tablespace_key = nullptr;
    my_free(key_type);
    return;
  }
  my_free(key_type);

  uint tablespace_key_version = 0;
  size_t tablespace_key_data_length = 0;

  if (parse_system_key(system_tablespace_key, key_len, &tablespace_key_version,
                       tablespace_key,
                       &tablespace_key_data_length) == nullptr) {
    my_free(system_tablespace_key);
    return;
  }
  my_free(system_tablespace_key);
  // Newly created key should have 1 assigned as its key version
  ut_ad(tablespace_key_version == 1);
  ut_ad(tablespace_key_data_length == KEY_LEN);
#endif
}

void Encryption::get_keyring_key(const char *key_name, byte **key,
                                 size_t *key_len) {
#ifndef UNIV_INNOCHECKSUM
  int ret;
  char *key_type = nullptr;
  /* We call key ring API to get master key here. */
  ret = my_key_fetch(key_name, &key_type, nullptr,
                     reinterpret_cast<void **>(key), key_len);

  if (key_type) {
    my_free(key_type);
  }

  if (ret) {
    *key = nullptr;
  }
#endif
}

bool Encryption::get_tablespace_key(uint key_id, const char *uuid,
                                    uint tablespace_key_version,
                                    byte **tablespace_key, size_t *key_len) {
  bool result = true;
#ifndef UNIV_INNOCHECKSUM
  char key_name[MASTER_KEY_NAME_MAX_LEN];

  fill_key_name(key_name, key_id, uuid, tablespace_key_version);

  get_keyring_key(key_name, tablespace_key, key_len);

  if (*tablespace_key == nullptr) {
    ib::error() << "Encryption can't find tablespace key_id = " << key_id
                << ", please check"
                << " the keyring plugin is loaded.";
    result = false;
  }

#ifdef UNIV_ENCRYPT_DEBUG
  if (*tablespace_key) {
    fprintf(stderr, "Fetched tablespace key:%s ", key_name);
    ut_print_buf(stderr, *tablespace_key, *key_len);
    fprintf(stderr, "\n");
  }
#endif /* DEBUG_TDE */
#endif
  return result;
}

void Encryption::get_latest_system_key(const char *system_key_name, byte **key,
                                       uint *key_version, size_t *key_length) {
#ifndef UNIV_INNOCHECKSUM
  size_t system_key_len = 0;
  uchar *system_key = nullptr;
  get_keyring_key(system_key_name, &system_key, &system_key_len);
  if (system_key == nullptr) {
    *key = nullptr;
    return;
  }

  parse_system_key(system_key, system_key_len, key_version, (uchar **)key,
                   key_length);
  my_free(system_key);
#endif
}

// tablespace_key_version as output parameter
void Encryption::get_latest_tablespace_key(uint key_id, const char *uuid,
                                           uint *tablespace_key_version,
                                           byte **tablespace_key) {
#ifndef UNIV_INNOCHECKSUM
  size_t key_len;
  char key_name[MASTER_KEY_NAME_MAX_LEN];

  fill_key_name(key_name, key_id, uuid);

  get_latest_system_key(key_name, tablespace_key, tablespace_key_version,
                        &key_len);

#ifdef UNIV_ENCRYPT_DEBUG
  if (*tablespace_key) {
    fprintf(stderr, "Fetched tablespace key:%s ", key_name);
    ut_print_buf(stderr, *tablespace_key, key_len);
    fprintf(stderr, "\n");
  }
#endif /* DEBUG_TDE */

#endif
}

bool Encryption::tablespace_key_exists(uint key_id, const char *uuid) {
  uint tablespace_key_version = 0;
  byte *tablespace_key = nullptr;

  get_latest_tablespace_key(key_id, uuid, &tablespace_key_version,
                            &tablespace_key);

  if (tablespace_key == nullptr) {
    return false;
  }

  my_free(tablespace_key);
  return true;
}

bool Encryption::tablespace_key_exists_or_create_new_one_if_does_not_exist(
    uint key_id, const char *uuid) {
  uint tablespace_key_version;
  byte *tablespace_key;

  get_latest_key_or_create(key_id, uuid, &tablespace_key_version,
                           &tablespace_key);

  if (tablespace_key == nullptr) {
    return false;
  }

  my_free(tablespace_key);
  return true;
}

bool Encryption::create_tablespace_key(EncryptionKeyId key_id) {
  byte *tablespace_key = nullptr;
  create_tablespace_key(&tablespace_key, key_id, server_uuid);
  if (tablespace_key == nullptr) {
    return true;
  }
  my_free(tablespace_key);
  return false;
}

void Encryption::get_latest_key_or_create(uint tablespace_key_id,
                                          const char *uuid,
                                          uint *tablespace_key_version,
                                          byte **tablespace_key) {
  get_latest_tablespace_key(tablespace_key_id, uuid, tablespace_key_version,
                            tablespace_key);
  if (*tablespace_key == nullptr) {
    create_tablespace_key(tablespace_key, tablespace_key_id, uuid);
    *tablespace_key_version = 1;
  }
}

/** Checks if keyring is installed and it is operational.
 *  This is done by trying to fetch/create
 *  dummy percona_keyring_test key
@return true if success */
bool Encryption::is_keyring_alive() {
  byte *keyring_test_key{nullptr};
  size_t key_len{0};
  const char *percona_keyring_test_key_name{"percona_keyring_test"};

  get_keyring_key(percona_keyring_test_key_name, &keyring_test_key, &key_len);

  if (keyring_test_key != nullptr) {
    my_free(keyring_test_key);
    return true;
  }

  int ret =
      my_key_generate(percona_keyring_test_key_name, "AES", nullptr, KEY_LEN);

  return (ret == 0) ? true : false;
}

bool Encryption::can_page_be_keyring_encrypted(ulint page_type) {
  switch (page_type) {
    case FIL_PAGE_TYPE_FSP_HDR:
    case FIL_PAGE_TYPE_XDES:
    case FIL_PAGE_RTREE:
      /* File space header, extent descriptor or spatial index
      are not encrypted. */
      return false;
  }
  return true;
}

bool Encryption::can_page_be_keyring_encrypted(byte *page) {
  ut_ad(page != nullptr);
  return can_page_be_keyring_encrypted(mach_read_from_2(page + FIL_PAGE_TYPE));
}

uint Encryption::encryption_get_latest_version(uint key_id, const char *uuid) {
#ifndef UNIV_INNOCHECKSUM
  uint tablespace_key_version = ENCRYPTION_KEY_VERSION_INVALID;
  byte *tablespace_key = nullptr;

  get_latest_tablespace_key(key_id, uuid, &tablespace_key_version,
                            &tablespace_key);

  if (tablespace_key == nullptr) return ENCRYPTION_KEY_VERSION_INVALID;

  my_free(tablespace_key);
  return tablespace_key_version;
#endif
  return ENCRYPTION_KEY_VERSION_INVALID;
}

void Encryption::create_master_key(byte **master_key) noexcept {
#ifndef UNIV_HOTBACKUP
  size_t key_len;
  char *key_type = nullptr;
  char key_name[MASTER_KEY_NAME_MAX_LEN];

  /* If uuid does not match with current server uuid,
  set uuid as current server uuid. */
  if (strcmp(s_uuid, server_uuid) != 0) {
    memcpy(s_uuid, server_uuid, sizeof(s_uuid) - 1);
  }

  /* Generate new master key */
  snprintf(key_name, MASTER_KEY_NAME_MAX_LEN, "%s-%s-" UINT32PF,
           MASTER_KEY_PREFIX, s_uuid, s_master_key_id + 1);

  /* We call key ring API to generate master key here. */
  int ret = my_key_generate(key_name, "AES", nullptr, KEY_LEN);

  /* We call key ring API to get master key here. */
  ret = my_key_fetch(key_name, &key_type, nullptr,
                     reinterpret_cast<void **>(master_key), &key_len);

  if (ret != 0 || *master_key == nullptr) {
    ib::error(ER_IB_MSG_831) << "Encryption can't find master key,"
                             << " please check the keyring plugin is loaded."
                             << " ret=" << ret;

    *master_key = nullptr;
  } else {
    ++s_master_key_id;
  }

  if (key_type != nullptr) {
    my_free(key_type);
  }
#endif /* !UNIV_HOTBACKUP */
}

void Encryption::get_master_key(uint32_t master_key_id, char *srv_uuid,
                                byte **master_key) noexcept {
  size_t key_len = 0;
  char key_name[MASTER_KEY_NAME_MAX_LEN];

  memset(key_name, 0x0, sizeof(key_name));

  if (srv_uuid != nullptr) {
    ut_ad(strlen(srv_uuid) > 0);

    snprintf(key_name, MASTER_KEY_NAME_MAX_LEN, "%s-%s-" UINT32PF,
             MASTER_KEY_PREFIX, srv_uuid, master_key_id);
  } else {
    /* For compitable with 5.7.11, we need to get master key with
    server id. */

    snprintf(key_name, MASTER_KEY_NAME_MAX_LEN, "%s-%lu-" UINT32PF,
             MASTER_KEY_PREFIX, server_id, master_key_id);
  }

#ifndef UNIV_HOTBACKUP
  /* We call key ring API to get master key here. */
  get_keyring_key(key_name, master_key, &key_len);
#else  /* !UNIV_HOTBACKUP */
  /* We call MEB to get master key here. */
  int ret = meb_key_fetch(key_name, &key_type, nullptr,
                          reinterpret_cast<void **>(master_key), &key_len);
  if (key_type != nullptr) {
    my_free(key_type);
  }

  if (ret != 0) {
    *master_key = nullptr;
  }
#endif /* !UNIV_HOTBACKUP */
  if (*master_key == nullptr) {
    ib::error(ER_IB_MSG_832) << "Encryption can't find master key,"
                             << " please check the keyring plugin is loaded.";
  }

#ifdef UNIV_ENCRYPT_DEBUG
  if (*master_key != nullptr) {
    std::ostringstream msg;

    ut_print_buf(msg, *master_key, key_len);

    ib::info(ER_IB_MSG_833)
        << "Fetched master key: " << master_key_id << "{" << msg.str() << "}";
  }
#endif /* UNIV_ENCRYPT_DEBUG */
}

void Encryption::get_master_key(uint32_t *master_key_id,
                                byte **master_key) noexcept {
#ifndef UNIV_HOTBACKUP
  int ret;
  size_t key_len;
  char *key_type = nullptr;
  char key_name[MASTER_KEY_NAME_MAX_LEN];
  extern ib_mutex_t master_key_id_mutex;
  bool key_id_locked = false;

  if (s_master_key_id == DEFAULT_MASTER_KEY_ID) {
    /* Take mutex as master_key_id is going to change. */
    mutex_enter(&master_key_id_mutex);
    key_id_locked = true;
  }

  memset(key_name, 0x0, sizeof(key_name));

  /* Check for s_master_key_id again, as a parallel rotation might have caused
  it to change. */
  if (s_master_key_id == DEFAULT_MASTER_KEY_ID) {
    memset(s_uuid, 0x0, sizeof(s_uuid));

    /* If m_master_key is DEFAULT_MASTER_KEY_ID, it means there's
    no encrypted tablespace yet. Generate the first master key now and store
    it to keyring. */
    memcpy(s_uuid, server_uuid, sizeof(s_uuid) - 1);

    /* Prepare the server s_uuid. */
    snprintf(key_name, MASTER_KEY_NAME_MAX_LEN, "%s-%s-1", MASTER_KEY_PREFIX,
             s_uuid);

    /* We call key ring API to generate master key here. */
    ret = my_key_generate(key_name, "AES", nullptr, KEY_LEN);

    /* We call key ring API to get master key here. */
    ret = my_key_fetch(key_name, &key_type, nullptr,
                       reinterpret_cast<void **>(master_key), &key_len);

    if (ret == 0 && *master_key != nullptr) {
      ++s_master_key_id;
      *master_key_id = s_master_key_id;
    }
#ifdef UNIV_ENCRYPT_DEBUG
    if (ret == 0 && *master_key != nullptr) {
      std::ostringstream msg;

      ut_print_buf(msg, *master_key, key_len);

      ib::info(ER_IB_MSG_834)
          << "Generated new master key: {" << msg.str() << "}";
    }
#endif /* UNIV_ENCRYPT_DEBUG */
  } else {
    *master_key_id = s_master_key_id;

    snprintf(key_name, MASTER_KEY_NAME_MAX_LEN, "%s-%s-" UINT32PF,
             MASTER_KEY_PREFIX, s_uuid, *master_key_id);

    /* We call key ring API to get master key here. */
    ret = my_key_fetch(key_name, &key_type, nullptr,
                       reinterpret_cast<void **>(master_key), &key_len);

    /* For compitability with 5.7.11, we need to try to get master
    key with server id when get master key with server uuid
    failure. */
    if (ret != 0 || *master_key == nullptr) {
      if (key_type != nullptr) {
        my_free(key_type);
      }

      snprintf(key_name, MASTER_KEY_NAME_MAX_LEN, "%s-%lu-" UINT32PF,
               MASTER_KEY_PREFIX, server_id, *master_key_id);

      ret = my_key_fetch(key_name, &key_type, nullptr,
                         reinterpret_cast<void **>(master_key), &key_len);
    }

#ifdef UNIV_ENCRYPT_DEBUG
    if (ret == 0 && *master_key != nullptr) {
      std::ostringstream msg;

      ut_print_buf(msg, *master_key, key_len);

      ib::info(ER_IB_MSG_835) << "Fetched master key: " << *master_key_id
                              << ": {" << msg.str() << "}";
    }
#endif /* UNIV_ENCRYPT_DEBUG */
  }

  if (ret != 0) {
    *master_key = nullptr;
    ib::error(ER_IB_MSG_836) << "Encryption can't find master key, please check"
                             << " the keyring plugin is loaded.";
  }

  if (key_type != nullptr) {
    my_free(key_type);
  }

  if (key_id_locked) {
    mutex_exit(&master_key_id_mutex);
  }

#endif /* !UNIV_HOTBACKUP */
}

/** Fill the encryption information.
@param[in]	key		encryption key
@param[in]	iv		encryption iv
@param[in,out]	encrypt_info	encryption information
@param[in]	is_boot		if it's for bootstrap
@return true if success */
bool Encryption::fill_encryption_info(byte *key, byte *iv, byte *encrypt_info,
                                      bool is_boot, bool encrypt_key) noexcept {
  byte *master_key = nullptr;
  uint32_t master_key_id = DEFAULT_MASTER_KEY_ID;
  bool is_default_master_key = false;

  /* Get master key from key ring. For bootstrap, we use a default
  master key which master_key_id is 0. */
  if (encrypt_key) {
    if (is_boot || strlen(server_uuid) == 0) {
      master_key = static_cast<byte *>(ut_zalloc_nokey(KEY_LEN));

      ut_ad(KEY_LEN >= sizeof(DEFAULT_MASTER_KEY));

      strcpy(reinterpret_cast<char *>(master_key), DEFAULT_MASTER_KEY);
      is_default_master_key = true;
      default_master_key_used = is_default_master_key;
    } else {
      get_master_key(&master_key_id, &master_key);

      if (master_key == nullptr) {
        return (false);
      }
    }
  }

  memset(encrypt_info, 0, INFO_SIZE);

  /* Use the new master key to encrypt the key. */
  ut_ad(encrypt_info != nullptr);
  auto ptr = encrypt_info;

  memcpy(ptr, KEY_MAGIC_V3, MAGIC_SIZE);

  ptr += MAGIC_SIZE;

  /* Write master key id. */
  mach_write_to_4(ptr, master_key_id);
  ptr += sizeof(uint32_t);

  /* Write server uuid. */
  memcpy(reinterpret_cast<char *>(ptr), s_uuid, sizeof(s_uuid));
  ptr += sizeof(s_uuid) - 1;

  /* We should never write empty UUID. Only exemption is for
  tablespaces when InnoDB is initializing (like system, temp, etc).
  These tablespaces UUID will be fixed by handlerton API after server
  generates uuid */
  ut_ad(!srv_is_uuid_ready || strlen(s_uuid) != 0);

  byte key_info[KEY_LEN * 2];

  memset(key_info, 0x0, sizeof(key_info));

  memcpy(key_info, key, KEY_LEN);

  memcpy(key_info + KEY_LEN, iv, KEY_LEN);

  if (encrypt_key) {
    /* Encrypt key and iv. */
    auto elen = my_aes_encrypt(key_info, sizeof(key_info), ptr, master_key,
                               KEY_LEN, my_aes_256_ecb, nullptr, false);

    if (elen == MY_AES_BAD_DATA) {
      my_free(master_key);
      return (false);
    }
  } else {
    /* Keep tablespace key unencrypted. Used by clone. */
    memcpy(ptr, key_info, sizeof(key_info));
  }

  ptr += sizeof(key_info);

  /* Write checksum bytes. */
  auto crc = ut_crc32(key_info, sizeof(key_info));

  mach_write_to_4(ptr, crc);

  if (encrypt_key) {
    ut_ad(master_key != nullptr);
    if (is_default_master_key) {
      ut_free(master_key);
    } else {
      my_free(master_key);
    }
  }

  return (true);
}

bool Encryption::fill_encryption_info(uint key_version, byte *iv,
                                      byte *encrypt_info) {
  byte *ptr = encrypt_info;
  ulint crc;
  memset(encrypt_info, 0, INFO_SIZE);
  memcpy(ptr, KEY_MAGIC_RK_V2, MAGIC_SIZE);
  ptr += MAGIC_SIZE;
  /* Write master key id. */
  mach_write_to_4(ptr, key_version);
  ptr += 4;
  /* Write server uuid. */
  memcpy(ptr, server_uuid, SERVER_UUID_LEN);
  ptr += SERVER_UUID_LEN;
  /* Write tablespace iv. */
  memcpy(ptr, iv, KEY_LEN);
  ptr += KEY_LEN;
  /* Write checksum bytes. */
  crc = ut_crc32(encrypt_info, KEY_LEN);
  mach_write_to_4(ptr, crc);
#ifdef UNIV_ENCRYPT_DEBUG
  fprintf(stderr, "Encrypting log with key version: %u\n", key_version);
#endif
  return true;
}

byte *Encryption::get_master_key_from_info(byte *encrypt_info, Version version,
                                           uint32_t *m_key_id, char *srv_uuid,
                                           byte **master_key) noexcept {
  byte *ptr;
  uint32 key_id;

  ptr = encrypt_info;
  *m_key_id = 0;

  /* Get master key id. */
  key_id = mach_read_from_4(ptr);
  ptr += sizeof(uint32);

  /* Handle different version encryption information. */
  switch (version) {
    case VERSION_1:
      /* For version 1, it's possible master key id
      occupied 8 bytes. */
      if (mach_read_from_4(ptr) == 0) {
        ptr += sizeof(uint32);
      }

      get_master_key(key_id, nullptr, master_key);
      if (*master_key == nullptr) {
        return (encrypt_info);
      }

      *m_key_id = key_id;
      return (ptr);

    case VERSION_2:
      /* For version 2, it's also possible master key id
      occupied 8 bytes. */
      if (mach_read_from_4(ptr) == 0) {
        ptr += sizeof(uint32);
      }

      /* Get server uuid. */
      memset(srv_uuid, 0, SERVER_UUID_LEN + 1);
      memcpy(srv_uuid, ptr, SERVER_UUID_LEN);

      ut_ad(strlen(srv_uuid) != 0);
      ptr += SERVER_UUID_LEN;

      /* Get master key. */
      get_master_key(key_id, srv_uuid, master_key);
      if (*master_key == nullptr) {
        return (encrypt_info);
      }

      *m_key_id = key_id;
      break;

    case VERSION_3:
      /* Get server uuid. */
      memset(srv_uuid, 0, SERVER_UUID_LEN + 1);
      memcpy(srv_uuid, ptr, SERVER_UUID_LEN);

      ptr += SERVER_UUID_LEN;

      if (key_id == 0) {
        /* When key_id is 0, which means it's the
        default master key for bootstrap. */
        *master_key = static_cast<byte *>(ut_zalloc_nokey(KEY_LEN));
        memcpy(*master_key, DEFAULT_MASTER_KEY, strlen(DEFAULT_MASTER_KEY));
        *m_key_id = 0;
      } else {
        ut_ad(strlen(srv_uuid) != 0);

        /* Get master key. */
        get_master_key(key_id, srv_uuid, master_key);
        if (*master_key == nullptr) {
          return (encrypt_info);
        }

        *m_key_id = key_id;
      }
      break;
  }

  ut_ad(*master_key != nullptr);

  return (ptr);
}

/** Decoding the encryption info from the first page of a tablespace.
@param[in,out]	key		key
@param[in,out]	iv		iv
@param[in]	encryption_info	encryption info
@return true if success */
bool Encryption::decode_encryption_info(byte *key, byte *iv,
                                        byte *encryption_info,
                                        bool decrypt_key) noexcept {
  byte *ptr;
  byte *master_key = nullptr;
  uint32_t master_key_id = DEFAULT_MASTER_KEY_ID;
  byte key_info[KEY_LEN * 2];
  ulint crc1;
  ulint crc2;
  char srv_uuid[SERVER_UUID_LEN + 1];
  Version version;

  ptr = encryption_info;

  /* For compatibility with 5.7.11, we need to handle the
  encryption information which created in this old version. */
  if (memcmp(ptr, KEY_MAGIC_V1, MAGIC_SIZE) == 0) {
    version = VERSION_1;
  } else if (memcmp(ptr, KEY_MAGIC_V2, MAGIC_SIZE) == 0) {
    version = VERSION_2;
  } else if (memcmp(ptr, KEY_MAGIC_V3, MAGIC_SIZE) == 0) {
    version = VERSION_3;
  } else {
    /* We don't report an error during recovery, since the
    encryption info maybe hasn't writen into datafile when
    the table is newly created. */
    if (recv_recovery_is_on()) {
      return (true);
    }

    ib::error(ER_IB_MSG_837) << "Failed to decrypt encryption information,"
                             << " found unexpected version of it!";
    return (false);
  }

  ptr += MAGIC_SIZE;

  if (decrypt_key) {
    /* Get master key by key id. */
    ptr = get_master_key_from_info(ptr, version, &master_key_id, srv_uuid,
                                   &master_key);

    /* If can't find the master key, return failure. */
    if (master_key == nullptr) {
      return (false);
    }

#ifdef UNIV_ENCRYPT_DEBUG
    {
      std::ostringstream msg;

      msg << "Master Key ID: " << master_key_id;
      msg << " hex: {";
      ut_print_buf_hex(msg, master_key, KEY_LEN);
      msg << "}";

      ib::info(ER_IB_MSG_838) << msg.str();
    }
#endif /* UNIV_ENCRYPT_DEBUG */

    /* Decrypt tablespace key and iv. */
    auto len = my_aes_decrypt(ptr, sizeof(key_info), key_info, master_key,
                              KEY_LEN, my_aes_256_ecb, nullptr, false);

    if (master_key_id == DEFAULT_MASTER_KEY_ID) {
      ut_free(master_key);
    } else {
      my_free(master_key);
    }

    /* If decryption failed, return error. */
    if (len == MY_AES_BAD_DATA) {
      return (false);
    }
  } else {
    ut_ad(version == VERSION_3);
    /* Skip master Key and server UUID*/
    ptr += sizeof(uint32);
    ptr += SERVER_UUID_LEN;

    /* Get tablespace key information. */
    memcpy(key_info, ptr, sizeof(key_info));
  }

  /* Check checksum bytes. */
  ptr += sizeof(key_info);

  crc1 = mach_read_from_4(ptr);
  crc2 = ut_crc32(key_info, sizeof(key_info));

  if (crc1 != crc2) {
    /* This check could fail only while decrypting key. */
    ut_ad(decrypt_key);

    ib::error(ER_IB_MSG_839)
        << "Failed to decrypt encryption information,"
        << " please check whether key file has been changed!";

    return (false);
  }

  /* Get tablespace key */
  memcpy(key, key_info, KEY_LEN);

  /* Get tablespace iv */
  memcpy(iv, key_info + KEY_LEN, KEY_LEN);

#ifdef UNIV_ENCRYPT_DEBUG
  {
    std::ostringstream msg;

    ut_print_buf_hex(msg, key, KEY_LEN);

    ib::info(ER_IB_MSG_840) << "Key: {" << msg.str() << "}";
  }
  {
    std::ostringstream msg;

    ut_print_buf_hex(msg, iv, KEY_LEN);
    ib::info(ER_IB_MSG_841) << "IV: {" << msg.str() << "}";
  }
#endif /* UNIV_ENCRYPT_DEBUG */

  if (decrypt_key && (s_master_key_id < master_key_id)) {
    s_master_key_id = master_key_id;
    memcpy(s_uuid, srv_uuid, sizeof(s_uuid) - 1);
  }

  return (true);
}

bool Encryption::is_encrypted_page(const byte *page) noexcept {
  ulint page_type = mach_read_from_2(page + FIL_PAGE_TYPE);

  return (page_type == FIL_PAGE_ENCRYPTED ||
          page_type == FIL_PAGE_COMPRESSED_AND_ENCRYPTED ||
          page_type == FIL_PAGE_ENCRYPTED_RTREE);
}

bool Encryption::is_encrypted_and_compressed(const byte *page) {
  ulint page_type = mach_read_from_2(page + FIL_PAGE_TYPE);

  return page_type == FIL_PAGE_COMPRESSED_AND_ENCRYPTED;
}

bool Encryption::is_encrypted_log(const byte *block) noexcept {
  return (log_block_get_encrypt_bit(block));
}

bool Encryption::encrypt_log_block(const IORequest &type, byte *src_ptr,
                                   byte *dst_ptr) noexcept {
  ulint len = 0;
  ulint main_len;
  ulint remain_len;
  byte remain_buf[MY_AES_BLOCK_SIZE * 2];

#ifdef UNIV_ENCRYPT_DEBUG
  {
    std::ostringstream msg;

    msg << "Encrypting block: " << log_block_get_hdr_no(src_ptr);
    msg << "{";
    ut_print_buf_hex(msg, src_ptr, OS_FILE_LOG_BLOCK_SIZE);
    msg << "}";

    ib::info(ER_IB_MSG_842) << msg.str();
  }
#endif /* UNIV_ENCRYPT_DEBUG */

  /* This is data size which need to encrypt. */
  const ulint unencrypted_trailer_size =
      (m_type == KEYRING) ? LOG_BLOCK_TRL_SIZE : 0;
  const ulint data_len =
      OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE - unencrypted_trailer_size;
  main_len = (data_len / MY_AES_BLOCK_SIZE) * MY_AES_BLOCK_SIZE;
  remain_len = data_len - main_len;

  /* Encrypt the block. */
  /* Copy the header as is. */
  memmove(dst_ptr, src_ptr, LOG_BLOCK_HDR_SIZE);
  ut_ad(memcmp(src_ptr, dst_ptr, LOG_BLOCK_HDR_SIZE) == 0);

  switch (m_type) {
    case NONE:
      ut_error;

    case KEYRING:
    case AES: {
      ut_ad(m_klen == KEY_LEN);

      auto elen = my_aes_encrypt(
          src_ptr + LOG_BLOCK_HDR_SIZE, static_cast<uint32>(main_len),
          dst_ptr + LOG_BLOCK_HDR_SIZE,
          reinterpret_cast<unsigned char *>(m_key), static_cast<uint32>(m_klen),
          my_aes_256_cbc, reinterpret_cast<unsigned char *>(m_iv), false);

      if (elen == MY_AES_BAD_DATA) {
        return (false);
      }

      len = static_cast<ulint>(elen);
      ut_ad(len == main_len);

      /* Copy remain bytes. */
      memcpy(dst_ptr + LOG_BLOCK_HDR_SIZE + len,
             src_ptr + LOG_BLOCK_HDR_SIZE + len,
             OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE - len);

      /* Encrypt the remain bytes. Since my_aes_encrypt
      request the content which need to encrypt is
      multiple of MY_AES_BLOCK_SIZE, but the block
      content is possiblly not, so, we need to handle
      the tail bytes first. */
      if (remain_len != 0) {
        remain_len = MY_AES_BLOCK_SIZE * 2;

        elen =
            my_aes_encrypt(dst_ptr + LOG_BLOCK_HDR_SIZE + data_len - remain_len,
                           static_cast<uint32>(remain_len), remain_buf,
                           reinterpret_cast<unsigned char *>(m_key),
                           static_cast<uint32>(m_klen), my_aes_256_cbc,
                           reinterpret_cast<unsigned char *>(m_iv), false);

        if (elen == MY_AES_BAD_DATA) {
          return (false);
        }

        memcpy(dst_ptr + LOG_BLOCK_HDR_SIZE + data_len - remain_len, remain_buf,
               remain_len);
      }

      break;
    }

    default:
      ut_error;
  }

  /* Set the encrypted flag. */
  log_block_set_encrypt_bit(dst_ptr, true);

  if (m_type == KEYRING) {
    const ulint crc = log_block_calc_checksum_crc32(dst_ptr);
    log_block_set_checksum(dst_ptr, crc + m_key_version);
  }

#ifdef UNIV_ENCRYPT_DEBUG
  {
    std::ostringstream os{};
    os << "Encrypted block " << log_block_get_hdr_no(dst_ptr) << "."
       << std::endl;
    ut_print_buf_hex(os, dst_ptr, OS_FILE_LOG_BLOCK_SIZE);
    os << std::endl;
    ib::info() << os.str();

    byte *check_buf =
        static_cast<byte *>(ut_malloc_nokey(OS_FILE_LOG_BLOCK_SIZE));
    byte *buf2 = static_cast<byte *>(ut_malloc_nokey(OS_FILE_LOG_BLOCK_SIZE));

    memcpy(check_buf, dst_ptr, OS_FILE_LOG_BLOCK_SIZE);
    log_block_set_encrypt_bit(check_buf, true);
    dberr_t err = decrypt_log(type, check_buf, OS_FILE_LOG_BLOCK_SIZE, buf2,
                              OS_FILE_LOG_BLOCK_SIZE);
    if (err != DB_SUCCESS ||
        memcmp(src_ptr, check_buf, OS_FILE_LOG_BLOCK_SIZE) != 0) {
      std::ostringstream msg{};
      ut_print_buf_hex(msg, src_ptr, OS_FILE_LOG_BLOCK_SIZE);
      ib::error() << msg.str();

      msg.seekp(0);
      ut_print_buf_hex(msg, check_buf, OS_FILE_LOG_BLOCK_SIZE);
      ib::fatal() << msg.str();
    }
    ut_free(buf2);
    ut_free(check_buf);
  }
#endif /* UNIV_ENCRYPT_DEBUG */

  return (true);
}

byte *Encryption::encrypt_log(const IORequest &type, byte *src, ulint src_len,
                              byte *dst, ulint *dst_len) noexcept {
  byte *src_ptr = src;
  byte *dst_ptr = dst;

  ut_ad(type.is_log());
  ut_ad(src_len % OS_FILE_LOG_BLOCK_SIZE == 0);
  ut_ad(m_type != NONE);

  /* Encrypt the log blocks one by one. */
  while (src_ptr != src + src_len) {
    if (!encrypt_log_block(type, src_ptr, dst_ptr)) {
      *dst_len = src_len;
      ib::error(ER_IB_MSG_843) << " Can't encrypt data of"
                               << " redo log";
      return (src);
    }

    src_ptr += OS_FILE_LOG_BLOCK_SIZE;
    dst_ptr += OS_FILE_LOG_BLOCK_SIZE;
  }

  return (dst);
}

bool Encryption::encrypt_low(const IORequest &type, byte *src, ulint src_len,
                             byte *dst, ulint *dst_len) noexcept {
  // Destination header might need to accommodate key_version and checksum after
  // encryption
  const uint16_t page_type = mach_read_from_2(src + FIL_PAGE_TYPE);
  const uint DST_HEADER_SIZE =
      (m_type == KEYRING && page_type == FIL_PAGE_COMPRESSED)
          ? FIL_PAGE_DATA + 8
          : FIL_PAGE_DATA;

  /* Shouldn't encrypt an already encrypted page. */
  ut_ad(!is_encrypted_page(src));

  /* This is data size which need to encrypt. */
  auto src_enc_len = src_len;

  /* In FIL_PAGE_VERSION_2, we encrypt the actual compressed data length. */
  if (page_type == FIL_PAGE_COMPRESSED) {
    src_enc_len =
        mach_read_from_2(src + FIL_PAGE_COMPRESS_SIZE_V1) + FIL_PAGE_DATA;
    /* Extend src_enc_len if needed */
    if (src_enc_len < MIN_ENCRYPTION_LEN) {
      src_enc_len = MIN_ENCRYPTION_LEN;
    }
    ut_a(src_enc_len <= src_len);
  }

  /* Only encrypt the data + trailer, leave the header alone */
  ulint data_len = 0;
  switch (m_type) {
    case NONE:
      ut_error;

    case KEYRING:
      // fallthrough

    case AES: {
      ut_ad(m_klen == KEY_LEN);
      ut_ad(m_iv != nullptr);

      /* Total length of the data to encrypt. */
      if (m_type == KEYRING && page_type == FIL_PAGE_COMPRESSED) {
        /* We need those 8 bytes for key_version and post-encryption checksum */
        data_len = src_enc_len - FIL_PAGE_DATA;
      } else if (m_type == KEYRING && !type.is_page_zip_compressed()) {
        /* For keyring encryption we do not encrypt last four bytes which are
           equal to the LSN bytes in header, so they are not encrypted
           anyways */
        data_len = src_enc_len - FIL_PAGE_DATA - 4;
      } else {
        data_len = src_enc_len - FIL_PAGE_DATA;
      }
      /* Server encryption functions expect input data to be in multiples
      of MY_AES_BLOCK SIZE. Therefore we encrypt the overlapping data of
      the chunk_len and trailer_len twice. First we encrypt the bigger
      chunk of data then we do the trailer. The trailer encryption block
      starts at 2 * MY_AES_BLOCK_SIZE bytes offset from the end of the
      enc_len.  During decryption we do the reverse of the above process. */
      ut_ad(data_len >= 2 * MY_AES_BLOCK_SIZE);

      const auto chunk_len = (data_len / MY_AES_BLOCK_SIZE) * MY_AES_BLOCK_SIZE;
      const auto remain_len = data_len - chunk_len;

      auto elen =
          my_aes_encrypt(src + FIL_PAGE_DATA, static_cast<uint32>(chunk_len),
                         dst + DST_HEADER_SIZE, reinterpret_cast<byte *>(m_key),
                         static_cast<uint32>(m_klen), my_aes_256_cbc,
                         reinterpret_cast<byte *>(m_iv), false);

      if (elen == MY_AES_BAD_DATA) {
        const auto page_id = page_get_page_id(src);

        ib::error(ER_IB_MSG_844) << " Can't encrypt data of page " << page_id;
        *dst_len = src_len;
        return false;
      }

      const auto len = static_cast<size_t>(elen);
      ut_a(len == chunk_len);

      /* Encrypt the trailing bytes. */
      if (remain_len != 0) {
        /* Copy remaining bytes and page tailer. */
        memcpy(dst + DST_HEADER_SIZE + len, src + FIL_PAGE_DATA + len,
               remain_len);

        constexpr size_t trailer_len = MY_AES_BLOCK_SIZE * 2;
        byte buf[trailer_len];

        elen = my_aes_encrypt(dst + DST_HEADER_SIZE + data_len - trailer_len,
                              static_cast<uint32>(trailer_len), buf,
                              reinterpret_cast<byte *>(m_key),
                              static_cast<uint32>(m_klen), my_aes_256_cbc,
                              reinterpret_cast<byte *>(m_iv), false);

        if (elen == MY_AES_BAD_DATA) {
          const auto page_id = page_get_page_id(src);

          ib::error(ER_IB_MSG_845) << " Can't encrypt data of page," << page_id;
          *dst_len = src_len;
          return false;
        }

        ut_a(static_cast<size_t>(elen) == trailer_len);

        memcpy(dst + DST_HEADER_SIZE + data_len - trailer_len, buf,
               trailer_len);
      }

      break;
    }

    default:
      ut_error;
  }

  /* Copy the header as is. */
  memmove(dst, src, FIL_PAGE_DATA);
  ut_ad(memcmp(src, dst, FIL_PAGE_DATA) == 0);

  /* Add encryption control information. Required for decrypting. */
  if (page_type == FIL_PAGE_COMPRESSED) {
    /* If the page is compressed, we don't need to save the
    original type, since it is done in compression already. */
    mach_write_to_2(dst + FIL_PAGE_TYPE, FIL_PAGE_COMPRESSED_AND_ENCRYPTED);
    ut_ad(memcmp(src + FIL_PAGE_TYPE + 2, dst + FIL_PAGE_TYPE + 2,
                 FIL_PAGE_DATA - FIL_PAGE_TYPE - 2) == 0);
  } else if (page_type == FIL_PAGE_RTREE) {
    /* If the page is R-tree page, we need to save original type. */
    mach_write_to_2(dst + FIL_PAGE_TYPE, FIL_PAGE_ENCRYPTED_RTREE);
  } else {
    mach_write_to_2(dst + FIL_PAGE_TYPE, FIL_PAGE_ENCRYPTED);
    mach_write_to_2(dst + FIL_PAGE_ORIGINAL_TYPE_V1, page_type);
  }

  /* Add padding 0 for unused portion */
  if (src_len > src_enc_len) {
    memset(dst + DST_HEADER_SIZE + data_len, 0,
           src_len - DST_HEADER_SIZE - data_len);
  }

  if (m_type == KEYRING) {
    /* assign key version to page and for master key to keyring rotation
     * assign post encryption checksum */
    m_checksum = 0;

    ut_ad(*dst_len == src_len);

    if (page_type == FIL_PAGE_COMPRESSED) {
      memset(
          dst + FIL_PAGE_DATA, 0,
          4);  // set the checksum data to 0s before the checksum is calculated
      mach_write_to_4(dst + FIL_PAGE_DATA + 4,
                      m_key_version);  // Add it here so it would be included in
                                       // the checksum
    }

#ifndef UNIV_INNOCHECKSUM
    if (m_encryption_rotation == Encryption_rotation::MASTER_KEY_TO_KEYRING) {
      if (type.is_page_zip_compressed())
        memcpy(dst + FIL_PAGE_ZIP_KEYRING_ENCRYPTION_MAGIC,
               ZIP_PAGE_KEYRING_ENCRYPTION_MAGIC,
               ZIP_PAGE_KEYRING_ENCRYPTION_MAGIC_LEN);

      uint page_size = *dst_len;
      if (page_type == FIL_PAGE_COMPRESSED) {
        page_size = static_cast<uint16_t>(
            mach_read_from_2(dst + FIL_PAGE_COMPRESS_SIZE_V1));
      } else if (type.is_page_zip_compressed()) {
        page_size = type.get_zip_page_physical_size();
      }
      m_checksum = fil_crypt_calculate_checksum(page_size, dst,
                                                type.is_page_zip_compressed());
      ut_ad(m_checksum != 0);
    }
#endif
    ut_ad(m_key_version != 0);  // Since we are encrypting key_version cannot be
                                // 0 (i.e. page unencrypted)

    mach_write_to_4(src + FIL_PAGE_ENCRYPTION_KEY_VERSION, m_key_version);

    if (page_type == FIL_PAGE_COMPRESSED) {
      if (m_checksum != 0) mach_write_to_4(dst + FIL_PAGE_DATA, m_checksum);
    } else if (!type.is_page_zip_compressed()) {
      mach_write_to_4(dst + FIL_PAGE_ENCRYPTION_KEY_VERSION, m_key_version);
      if (m_checksum != 0) mach_write_to_4(dst + *dst_len - 4, m_checksum);
    } else if (type.is_page_zip_compressed()) {
      mach_write_to_4(dst + FIL_PAGE_ENCRYPTION_KEY_VERSION, m_key_version);
      ut_ad(m_key_version != 0);
    }
#ifdef UNIV_ENCRYPT_DEBUG
    ut_ad(type.is_page_zip_compressed() ||
          fil_space_verify_crypt_checksum(
              dst, *dst_len, type.is_page_zip_compressed(),
              type.is_compressed()));  // This works only for not zipped
                                       // compressed pages
#endif
  }

  *dst_len = src_len;

  return (true);
}

byte *Encryption::encrypt(const IORequest &type, byte *src, ulint src_len,
                          byte *dst, ulint *dst_len) noexcept {
  /* For encrypting redo log, take another way. */
  ut_ad(!type.is_log());

#ifdef UNIV_ENCRYPT_DEBUG
  {
    ulint space_id = mach_read_from_4(src + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);
    ulint page_no = mach_read_from_4(src + FIL_PAGE_OFFSET);

    fprintf(stderr, "Encrypting page:%lu.%lu len:%lu\n", space_id, page_no,
            src_len);
    ut_print_buf(stderr, m_key, 32);
    ut_print_buf(stderr, m_iv, 32);
  }
#endif /* UNIV_ENCRYPT_DEBUG */

  ut_ad(m_type != NONE);

  if (!encrypt_low(type, src, src_len, dst, dst_len)) {
    return (src);
  }

#ifdef UNIV_ENCRYPT_DEBUG
  {
    byte *check_buf = static_cast<byte *>(ut_malloc_nokey(src_len));
    byte *buf2 = static_cast<byte *>(ut_malloc_nokey(src_len));

    memcpy(check_buf, dst, src_len);

    dberr_t err = decrypt(type, check_buf, src_len, buf2, src_len);
    if (err != DB_SUCCESS ||
        memcmp(src + FIL_PAGE_DATA, check_buf + FIL_PAGE_DATA,
               src_len - FIL_PAGE_DATA) != 0) {
      ut_print_buf(stderr, src, src_len);
      ut_print_buf(stderr, check_buf, src_len);
      ut_ad(0);
    }
    ut_free(buf2);
    ut_free(check_buf);
  }
#endif /* UNIV_ENCRYPT_DEBUG */

#if !defined(UNIV_INNOCHECKSUM)
  srv_stats.pages_encrypted.inc();
#endif
  return dst;
}

dberr_t Encryption::decrypt_log_block(const IORequest &type, byte *src,
                                      byte *dst) noexcept {
  ulint main_len;
  ulint remain_len;
  byte remain_buf[MY_AES_BLOCK_SIZE * 2];
  byte *ptr = src;

  /* This is data size which need to encrypt. */
  const ulint unencrypted_trailer_size =
      (m_type == KEYRING) ? LOG_BLOCK_TRL_SIZE : 0;
  const ulint data_len =
      OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE - unencrypted_trailer_size;

  main_len = (data_len / MY_AES_BLOCK_SIZE) * MY_AES_BLOCK_SIZE;
  remain_len = data_len - main_len;

  ptr += LOG_BLOCK_HDR_SIZE;
  switch (m_type) {
    case KEYRING: {
      const ulint block_crc = log_block_calc_checksum_crc32(src);
      const ulint written_crc = log_block_get_checksum(src);

      const ulint enc_key_version = written_crc - block_crc;

      if (m_key_version != enc_key_version &&
          enc_key_version != REDO_LOG_ENCRYPT_NO_VERSION) {
        redo_log_key *mkey = redo_log_key_mgr.load_key_version(
            nullptr, m_key_id_uuid, enc_key_version);
        m_key_version = mkey->version;
        m_key = reinterpret_cast<unsigned char *>(mkey->key);
      }
    }
      /* FALLTHROUGH */

    case AES: {
      lint elen;

      /* First decrypt the last 2 blocks data of data, since
      data is no block aligned. */
      if (remain_len != 0) {
        ut_ad(m_klen == KEY_LEN);

        remain_len = MY_AES_BLOCK_SIZE * 2;

        /* Copy the last 2 blocks. */
        memcpy(remain_buf, ptr + data_len - remain_len, remain_len);

        elen = my_aes_decrypt(remain_buf, static_cast<uint32>(remain_len),
                              dst + data_len - remain_len,
                              reinterpret_cast<unsigned char *>(m_key),
                              static_cast<uint32>(m_klen), my_aes_256_cbc,
                              reinterpret_cast<unsigned char *>(m_iv), false);
        if (elen == MY_AES_BAD_DATA) {
          return (DB_IO_DECRYPT_FAIL);
        }

        /* Copy the other data bytes to temp area. */
        memcpy(dst, ptr, data_len - remain_len);
      } else {
        ut_ad(data_len == main_len);

        /* Copy the data bytes to temp area. */
        memcpy(dst, ptr, data_len);
      }

      /* Then decrypt the main data */
      elen = my_aes_decrypt(dst, static_cast<uint32>(main_len), ptr,
                            reinterpret_cast<unsigned char *>(m_key),
                            static_cast<uint32>(m_klen), my_aes_256_cbc,
                            reinterpret_cast<unsigned char *>(m_iv), false);
      if (elen == MY_AES_BAD_DATA) {
        return (DB_IO_DECRYPT_FAIL);
      }

      ut_ad(static_cast<ulint>(elen) == main_len);

      /* Copy the remain bytes. */
      memcpy(ptr + main_len, dst + main_len, data_len - main_len);

      break;
    }

    default:
      ib::error(ER_IB_MSG_846)
          << "Encryption algorithm support missing: " << to_string(m_type);
      return (DB_UNSUPPORTED);
  }

  ptr -= LOG_BLOCK_HDR_SIZE;

#ifdef UNIV_ENCRYPT_DEBUG
  {
    std::ostringstream msg{};
    msg << "Decrypted block " << log_block_get_hdr_no(ptr) << "." << std::endl;
    ut_print_buf_hex(msg, ptr, OS_FILE_LOG_BLOCK_SIZE);
    msg << std::endl;
    ib::info() << msg.str();
  }
#endif

  /* Reset the encrypted flag. */
  log_block_set_encrypt_bit(ptr, false);

  if (m_type == KEYRING) {
    const ulint crc = log_block_calc_checksum_crc32(ptr);
    log_block_set_checksum(ptr, crc);
  }

  return (DB_SUCCESS);
}

dberr_t Encryption::decrypt_log(const IORequest &type, byte *src, ulint src_len,
                                byte *dst, ulint dst_len) noexcept {
  file::Block *block;
  byte *ptr = src;
  dberr_t ret;

  /* Do nothing if it's not a log request. */
  ut_ad(type.is_log());

  /* The caller doesn't know what to expect */
  if (dst == nullptr) {
    block = os_alloc_block();
    dst = block->m_ptr;
  } else {
    block = nullptr;
  }

  /* Decrypt the log blocks one by one. */
  while (ptr != src + src_len) {
#ifdef UNIV_ENCRYPT_DEBUG
    {
      std::ostringstream msg;

      msg << "Decrypting block: " << log_block_get_hdr_no(ptr) << std::endl;
      msg << "data={" << std::endl;
      ut_print_buf_hex(msg, ptr, OS_FILE_LOG_BLOCK_SIZE);
      msg << std::endl << "}";

      ib::info(ER_IB_MSG_847) << msg.str();
    }
#endif /* UNIV_ENCRYPT_DEBUG */

    /* If it's not an encrypted block, skip it. */
    if (!is_encrypted_log(ptr)) {
      ptr += OS_FILE_LOG_BLOCK_SIZE;
      continue;
    }

    /* Decrypt block */
    ret = decrypt_log_block(type, ptr, dst);
    if (ret != DB_SUCCESS) {
      if (block != nullptr) {
        os_free_block(block);
      }

      return (ret);
    }

    ptr += OS_FILE_LOG_BLOCK_SIZE;
  }

  if (block != nullptr) {
    os_free_block(block);
  }

  return (DB_SUCCESS);
}

dberr_t Encryption::decrypt(const IORequest &type, byte *src, ulint src_len,
                            byte *dst, ulint dst_len) noexcept {
  ulint data_len;
  ulint main_len;
  ulint remain_len;
  ulint original_type;
  byte remain_buf[MY_AES_BLOCK_SIZE * 2];
  file::Block *block;

  /* If the page is encrypted, then we need key to decrypt it. */
  if (is_encrypted_page(src) && m_type == NONE) {
    return DB_IO_DECRYPT_FAIL;
  }

  if (!is_encrypted_page(src) || m_type == NONE) {
    /* There is nothing we can do. */
    return DB_SUCCESS;
  }

  /* For compressed page, we need to get the compressed size
  for decryption */
  const ulint page_type = mach_read_from_2(src + FIL_PAGE_TYPE);

  /* Actual length of the compressed data */
  uint16_t z_len = 0;

  if (page_type == FIL_PAGE_COMPRESSED_AND_ENCRYPTED) {
    z_len = mach_read_from_2(src + FIL_PAGE_COMPRESS_SIZE_V1);
    src_len = z_len + FIL_PAGE_DATA;

    Compression::meta_t header;
    Compression::deserialize_header(src, &header);
    if (header.m_version == Compression::FIL_PAGE_VERSION_1) {
      src_len = ut_calc_align(src_len, type.block_size());
    } else {
      /* Extend src_len if needed */
      if (src_len < MIN_ENCRYPTION_LEN) {
        src_len = MIN_ENCRYPTION_LEN;
      }
    }
  }

#ifdef UNIV_ENCRYPT_DEBUG
  const page_id_t page_id(
      mach_read_from_4(src + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID),
      mach_read_from_4(src + FIL_PAGE_OFFSET));

  {
    std::ostringstream msg;

    msg << "Decrypting page: " << page_id << " len: " << src_len << std::endl;
    msg << "key={";
    ut_print_buf(msg, m_key, 32);
    msg << "}" << std::endl << "iv= {";
    ut_print_buf(msg, m_iv, 32);
    msg << "}";

    ib::info(ER_IB_MSG_848) << msg.str();
  }
#endif /* UNIV_ENCRYPT_DEBUG */

  const uint HEADER_SIZE =
      (m_type == KEYRING && page_type == FIL_PAGE_COMPRESSED_AND_ENCRYPTED)
          ? FIL_PAGE_DATA + 8
          : FIL_PAGE_DATA;
  original_type =
      static_cast<uint16_t>(mach_read_from_2(src + FIL_PAGE_ORIGINAL_TYPE_V1));

  byte *ptr = src + HEADER_SIZE;

  /* The caller doesn't know what to expect */
  if (dst == nullptr) {
    block = os_alloc_block();
    dst = block->m_ptr;
  } else {
    block = nullptr;
  }

  ut_ad(m_key != nullptr);

  data_len = src_len - HEADER_SIZE;
  if (m_type == Encryption::KEYRING &&
      page_type == FIL_PAGE_COMPRESSED_AND_ENCRYPTED) {
    // There are 8 bytes after the header used for key_version and checksum
    data_len += 8;
  } else if (page_type == FIL_PAGE_ENCRYPTED && m_type == Encryption::KEYRING &&
             !type.is_page_zip_compressed()) {
    data_len -= 4;  // Last 4 bytes are not encrypted
  }

  main_len = (data_len / MY_AES_BLOCK_SIZE) * MY_AES_BLOCK_SIZE;
  remain_len = data_len - main_len;

  switch (m_type) {
    case KEYRING:
    case AES: {
      lint elen;

      /* First decrypt the last 2 blocks data of data, since
      data is no block aligned. */
      if (remain_len != 0) {
        ut_ad(m_klen == KEY_LEN);
        ut_ad(m_iv != nullptr);

        remain_len = MY_AES_BLOCK_SIZE * 2;

        /* Copy the last 2 blocks. */
        memcpy(remain_buf, ptr + data_len - remain_len, remain_len);

        elen = my_aes_decrypt(remain_buf, static_cast<uint32>(remain_len),
                              dst + data_len - remain_len,
                              reinterpret_cast<unsigned char *>(m_key),
                              static_cast<uint32>(m_klen), my_aes_256_cbc,
                              reinterpret_cast<unsigned char *>(m_iv), false);

        if (elen == MY_AES_BAD_DATA) {
          if (block != nullptr) {
            os_free_block(block);
          }

          return (DB_IO_DECRYPT_FAIL);
        }

        ut_ad(static_cast<ulint>(elen) == remain_len);

        /* Copy the other data bytes to temp area. */
        memcpy(dst, ptr, data_len - remain_len);
      } else {
        ut_ad(data_len == main_len);

        /* Copy the data bytes to temp area. */
        memcpy(dst, ptr, data_len);
      }

      if (m_type == KEYRING && page_type == FIL_PAGE_COMPRESSED_AND_ENCRYPTED) {
        ptr -= 8;  // This much is unused as it was previously used by key
                   // version and encrypted checksum
        // It is not needed - overwrite this with decrypted data
        memset(ptr + data_len, 0, 8);
      }

      /* Then decrypt the main data */
      elen = my_aes_decrypt(dst, static_cast<uint32>(main_len), ptr,
                            reinterpret_cast<unsigned char *>(m_key),
                            static_cast<uint32>(m_klen), my_aes_256_cbc,
                            reinterpret_cast<unsigned char *>(m_iv), false);
      if (elen == MY_AES_BAD_DATA) {
        if (block != nullptr) {
          os_free_block(block);
        }

        return (DB_IO_DECRYPT_FAIL);
      }

      ut_ad(static_cast<ulint>(elen) == main_len);

      /* Copy the remain bytes. */
      memcpy(ptr + main_len, dst + main_len, data_len - main_len);

      break;
    }

    default:
      if (!type.is_dblwr()) {
        ib::error(ER_IB_MSG_849)
            << "Encryption algorithm support missing: " << to_string(m_type);
      }

      if (block != nullptr) {
        os_free_block(block);
      }

      return (DB_UNSUPPORTED);
  }

  if (m_type == KEYRING && page_type != FIL_PAGE_COMPRESSED_AND_ENCRYPTED &&
      !type.is_page_zip_compressed()) {
    // restore LSN
    memcpy(src + src_len - FIL_PAGE_END_LSN_OLD_CHKSUM + 4,
           src + FIL_PAGE_LSN + 4, 4);
  }

  /* Restore the original page type. If it's a compressed and
  encrypted page, just reset it as compressed page type, since
  we will do uncompress later. */

  if (page_type == FIL_PAGE_ENCRYPTED) {
    mach_write_to_2(src + FIL_PAGE_TYPE, original_type);
  } else if (page_type == FIL_PAGE_ENCRYPTED_RTREE) {
    mach_write_to_2(src + FIL_PAGE_TYPE, FIL_PAGE_RTREE);
  } else {
    ut_ad(page_type == FIL_PAGE_COMPRESSED_AND_ENCRYPTED);
    mach_write_to_2(src + FIL_PAGE_TYPE, FIL_PAGE_COMPRESSED);
  }

  // mark orignal page_type as encrypted - so that when checksum check fail - we
  // will be able to report that if failed because decryption failed
  if (original_type != FIL_PAGE_TYPE_ALLOCATED &&
      page_type != FIL_PAGE_COMPRESSED_AND_ENCRYPTED) {
    mach_write_to_2(src + FIL_PAGE_ORIGINAL_TYPE_V1, FIL_PAGE_ENCRYPTED);
  }

  if (block != nullptr) {
    os_free_block(block);
  }

#ifdef UNIV_DEBUG
  {
    /* Check if all the padding bytes are zeroes. */
    if (page_type == FIL_PAGE_COMPRESSED_AND_ENCRYPTED) {
      uint32_t padding = src_len - FIL_PAGE_DATA - z_len;
      for (uint32_t i = 0; i < padding; ++i) {
        byte *pad = src + z_len + FIL_PAGE_DATA + i;
        ut_ad(*pad == 0x0);
      }
    }
  }
#endif /* UNIV_DEBUG */

#ifdef UNIV_ENCRYPT_DEBUG
  ib::info(ER_IB_MSG_850) << "Decrypted page: " << page_id;
#endif /* UNIV_ENCRYPT_DEBUG */

  DBUG_EXECUTE_IF("ib_crash_during_decrypt_page", DBUG_SUICIDE(););

#if !defined(UNIV_INNOCHECKSUM)
  srv_stats.pages_decrypted.inc();
#endif
  return (DB_SUCCESS);
}

#ifndef UNIV_HOTBACKUP
bool Encryption::check_keyring() noexcept {
  bool ret = false;
  byte *master_key = nullptr;

  /* During bootstrap, default master key is used */
  if (s_master_key_id == DEFAULT_MASTER_KEY_ID) {
    static bool checked = false;
    if (checked) {
      return true;
    }

    /* Generate/Fetch/Delete a dummy master key to confirm keyring is up and
    running. */
    size_t key_len;
    char *key_type = nullptr;
    char key_name[MASTER_KEY_NAME_MAX_LEN];

    key_name[sizeof(DEFAULT_MASTER_KEY)] = 0;

    strncpy(key_name, DEFAULT_MASTER_KEY, sizeof(key_name));

    /* We call key ring API to generate master key here. */
    int my_ret = my_key_generate(key_name, "AES", nullptr, KEY_LEN);

    /* We call key ring API to get master key here. */
    my_ret = my_key_fetch(key_name, &key_type, nullptr,
                          reinterpret_cast<void **>(&master_key), &key_len);

    if (my_ret != 0) {
      ib::error(ER_IB_MSG_851) << "Check keyring plugin fail, please check the"
                               << " keyring plugin is loaded.";
    } else {
      my_key_remove(key_name, nullptr);
      ret = true;
      checked = true;
    }

    if (key_type != nullptr) {
      my_free(key_type);
    }

    if (master_key != nullptr) {
      my_free(master_key);
    }
  } else {
    uint32_t master_key_id;

    Encryption::get_master_key(&master_key_id, &master_key);
    if (master_key != nullptr) {
      my_free(master_key);
      ret = true;
    }
  }

  return (ret);
}
#endif /* !UNIV_HOTBACKUP */

Encryption::Type Encryption::get_type() const { return m_type; }

void Encryption::set_type(Encryption::Type type) { m_type = type; }

byte *Encryption::get_key() const { return m_key; }

std::map<uint, byte *> *Encryption::get_key_versions_cache() const {
  return m_key_versions_cache;
}

void Encryption::set_key(byte *key) { m_key = key; }

ulint Encryption::get_key_length() const { return m_klen; }

void Encryption::set_key_length(ulint klen) { m_klen = klen; }

byte *Encryption::get_initial_vector() const { return m_iv; }

void Encryption::set_initial_vector(byte *iv) { m_iv = iv; }

byte *Encryption::get_tablespace_key() const { return m_tablespace_key; }

void Encryption::set_tablespace_key(byte *tablespace_key) {
  m_tablespace_key = tablespace_key;
}

const char *Encryption::get_key_id_uuid() const { return m_key_id_uuid; }

void Encryption::set_key_id_uuid(const char *key_id_uuid) {
  if (key_id_uuid == nullptr) {
    m_key_id_uuid[0] = '\0';
  } else {
    memcpy(m_key_id_uuid, key_id_uuid, SERVER_UUID_LEN);
    m_key_id_uuid[SERVER_UUID_LEN] = '\0';
  }
}

ulint Encryption::get_key_version() const { return m_key_version; }

void Encryption::set_key_version(ulint key_version) {
  m_key_version = key_version;
}

ulint Encryption::get_key_id() const { return m_key_id; }

void Encryption::set_key_id(ulint key_id) { m_key_id = key_id; }

Encryption_rotation Encryption::get_encryption_rotation() const {
  return m_encryption_rotation;
}

void Encryption::set_encryption_rotation(
    Encryption_rotation encryption_rotation) {
  m_encryption_rotation = encryption_rotation;
}

uint32_t Encryption::get_master_key_id() { return s_master_key_id; }
