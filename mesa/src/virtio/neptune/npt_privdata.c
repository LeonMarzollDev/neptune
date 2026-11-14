/*
 * SPDX-License-Identifier: MIT
 */
#include "npt_privdata.h"

#include <stdlib.h>
#include <string.h>

#define NPT_PRIVDATA_BUCKETS 1024u
#define NPT_PRIVDATA_GUIDS 256u

struct npt_privdata_entry {
   struct npt_privdata_entry *next;
   uint64_t object_id;
   GUID guid;
   bool opaque;
   UINT size;
   uint8_t data[];
};

static SRWLOCK npt_privdata_lock = SRWLOCK_INIT;
static struct npt_privdata_entry *npt_privdata_buckets[NPT_PRIVDATA_BUCKETS];

/* Every GUID any object was ever given data for.  A GUID missing here was
 * never set anywhere, so NOT_FOUND is certain without asking the host.  When
 * the table is full, lookups of unknown GUIDs go to the host. */
static GUID npt_privdata_seen[NPT_PRIVDATA_GUIDS];
static uint32_t npt_privdata_seen_count;
static bool npt_privdata_seen_overflow;

/* WKPDID_D3DDebugObjectName and WKPDID_D3DDebugObjectNameW: SetName stores
 * them on the host. */
static const GUID npt_privdata_name_a = {
   0x429b8c22, 0x9188, 0x4b0c, { 0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00 } };
static const GUID npt_privdata_name_w = {
   0x4cca5fd8, 0x921f, 0x42c8, { 0x85, 0x66, 0x70, 0xca, 0xf2, 0xa9, 0xb7, 0x41 } };

static uint32_t
npt_privdata_bucket(uint64_t object_id)
{
   object_id ^= object_id >> 33;
   object_id *= 0xff51afd7ed558ccdull;
   object_id ^= object_id >> 33;
   return (uint32_t)object_id & (NPT_PRIVDATA_BUCKETS - 1);
}

static bool
npt_privdata_guid_seen_locked(const GUID *guid)
{
   for (uint32_t i = 0; i < npt_privdata_seen_count; i++) {
      if (!memcmp(&npt_privdata_seen[i], guid, sizeof(*guid)))
         return true;
   }
   return false;
}

static void
npt_privdata_note_guid_locked(const GUID *guid)
{
   if (npt_privdata_guid_seen_locked(guid))
      return;
   if (npt_privdata_seen_count == NPT_PRIVDATA_GUIDS) {
      npt_privdata_seen_overflow = true;
      return;
   }
   npt_privdata_seen[npt_privdata_seen_count++] = *guid;
}

static struct npt_privdata_entry **
npt_privdata_find_locked(uint64_t object_id, const GUID *guid)
{
   struct npt_privdata_entry **link =
      &npt_privdata_buckets[npt_privdata_bucket(object_id)];
   for (; *link; link = &(*link)->next) {
      if ((*link)->object_id == object_id &&
          !memcmp(&(*link)->guid, guid, sizeof(*guid)))
         return link;
   }
   return NULL;
}

static void
npt_privdata_store(uint64_t object_id, const GUID *guid, bool opaque,
                   UINT size, const void *data)
{
   if (!guid)
      return;

   struct npt_privdata_entry *entry = NULL;
   const bool remove = !opaque && (!data || !size);
   if (!remove) {
      entry = malloc(sizeof(*entry) + (opaque ? 0 : size));
      if (entry) {
         entry->object_id = object_id;
         entry->guid = *guid;
         entry->opaque = opaque;
         entry->size = opaque ? 0 : size;
         if (!opaque)
            memcpy(entry->data, data, size);
      } else {
         /* No copy: the host still has the data, so make lookups ask it. */
         opaque = true;
      }
   }

   AcquireSRWLockExclusive(&npt_privdata_lock);
   npt_privdata_note_guid_locked(guid);
   struct npt_privdata_entry **link = npt_privdata_find_locked(object_id, guid);
   if (link) {
      struct npt_privdata_entry *old = *link;
      *link = old->next;
      free(old);
   }
   if (entry) {
      struct npt_privdata_entry **head =
         &npt_privdata_buckets[npt_privdata_bucket(object_id)];
      entry->next = *head;
      *head = entry;
   } else if (!remove) {
      /* Allocation failed: forget the GUID is answerable locally. */
      npt_privdata_seen_overflow = true;
   }
   ReleaseSRWLockExclusive(&npt_privdata_lock);
}

void
npt_privdata_set(uint64_t object_id, const GUID *guid, UINT size,
                 const void *data)
{
   npt_privdata_store(object_id, guid, false, size, data);
}

void
npt_privdata_set_opaque(uint64_t object_id, const GUID *guid)
{
   npt_privdata_store(object_id, guid, true, 0, NULL);
}

void
npt_privdata_note_name(uint64_t object_id)
{
   npt_privdata_store(object_id, &npt_privdata_name_a, true, 0, NULL);
   npt_privdata_store(object_id, &npt_privdata_name_w, true, 0, NULL);
}

bool
npt_privdata_get(uint64_t object_id, const GUID *guid, UINT *pDataSize,
                 void *pData, HRESULT *hr)
{
   if (!guid || !pDataSize)
      return false;

   bool answered = false;
   AcquireSRWLockShared(&npt_privdata_lock);
   struct npt_privdata_entry **link = npt_privdata_find_locked(object_id, guid);
   if (link) {
      const struct npt_privdata_entry *entry = *link;
      if (!entry->opaque) {
         if (!pData) {
            *pDataSize = entry->size;
            *hr = NPT_S_OK;
         } else if (*pDataSize < entry->size) {
            *pDataSize = entry->size;
            *hr = NPT_DXGI_ERROR_MORE_DATA;
         } else {
            memcpy(pData, entry->data, entry->size);
            *pDataSize = entry->size;
            *hr = NPT_S_OK;
         }
         answered = true;
      }
   } else if (!npt_privdata_seen_overflow &&
              !npt_privdata_guid_seen_locked(guid)) {
      /* Never set on any object: the host cannot have it either. */
      *pDataSize = 0;
      *hr = NPT_DXGI_ERROR_NOT_FOUND;
      answered = true;
   }
   ReleaseSRWLockShared(&npt_privdata_lock);
   return answered;
}

void
npt_privdata_forget(uint64_t object_id)
{
   AcquireSRWLockExclusive(&npt_privdata_lock);
   struct npt_privdata_entry **link =
      &npt_privdata_buckets[npt_privdata_bucket(object_id)];
   while (*link) {
      struct npt_privdata_entry *entry = *link;
      if (entry->object_id == object_id) {
         *link = entry->next;
         free(entry);
      } else {
         link = &entry->next;
      }
   }
   ReleaseSRWLockExclusive(&npt_privdata_lock);
}
