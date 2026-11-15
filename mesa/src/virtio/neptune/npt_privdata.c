/*
 * SPDX-License-Identifier: MIT
 */
#include "npt_privdata.h"

#include "npt_com.h"

#include <stdlib.h>
#include <string.h>

struct npt_privdata_entry {
   struct npt_privdata_entry *next;
   uint64_t object_id;
   GUID guid;
   UINT size;
   uint8_t data[];
};

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

static struct npt_privdata *
npt_privdata_of(struct npt_com_base *com)
{
   return com->base.device ? &com->base.device->privdata : NULL;
}

static struct npt_privdata_guid *
npt_privdata_find_guid_locked(struct npt_privdata *pd, const GUID *guid)
{
   for (uint32_t i = 0; i < pd->guid_count; i++) {
      if (!memcmp(&pd->guids[i].guid, guid, sizeof(*guid)))
         return &pd->guids[i];
   }
   return NULL;
}

/* NULL once the table is full: the host answers that GUID. */
static struct npt_privdata_guid *
npt_privdata_note_guid_locked(struct npt_privdata *pd, const GUID *guid)
{
   struct npt_privdata_guid *g = npt_privdata_find_guid_locked(pd, guid);
   if (g)
      return g;
   if (pd->guid_count == NPT_PRIVDATA_GUIDS) {
      pd->guid_overflow = true;
      return NULL;
   }
   g = &pd->guids[pd->guid_count++];
   g->guid = *guid;
   g->host_only = false;
   return g;
}

static struct npt_privdata_entry **
npt_privdata_find_locked(struct npt_privdata *pd, uint64_t object_id,
                         const GUID *guid)
{
   struct npt_privdata_entry **link =
      &pd->buckets[npt_privdata_bucket(object_id)];
   for (; *link; link = &(*link)->next) {
      if ((*link)->object_id == object_id &&
          !memcmp(&(*link)->guid, guid, sizeof(*guid)))
         return link;
   }
   return NULL;
}

/* A write through `com`: `entry` replaces its copy (NULL removes it), or,
 * with `host_only`, the host alone knows the GUID from now on. */
static void
npt_privdata_write(struct npt_com_base *com, const GUID *guid,
                   struct npt_privdata_entry *entry, bool host_only)
{
   struct npt_privdata *pd = npt_privdata_of(com);
   if (!pd) {
      free(entry);
      return;
   }

   struct npt_privdata_entry *old = NULL;
   AcquireSRWLockExclusive(&pd->lock);
   struct npt_privdata_guid *g = npt_privdata_note_guid_locked(pd, guid);
   if (com->base.privdata_sole) {
      struct npt_privdata_entry **link =
         npt_privdata_find_locked(pd, com->base.id, guid);
      if (link) {
         old = *link;
         *link = old->next;
      }
   }
   if (g && host_only) {
      g->host_only = true;
   } else if (g && entry && !g->host_only) {
      struct npt_privdata_entry **head =
         &pd->buckets[npt_privdata_bucket(com->base.id)];
      entry->next = *head;
      *head = entry;
      entry = NULL;
   }
   ReleaseSRWLockExclusive(&pd->lock);
   free(old);
   free(entry);
}

void
npt_privdata_mark_sole(void *self)
{
   ((struct npt_com_base *)self)->base.privdata_sole = true;
}

void
npt_privdata_set(void *self, const GUID *guid, UINT size, const void *data)
{
   struct npt_com_base *com = self;
   if (!guid)
      return;

   /* Another id of this object may hold the GUID too: only the host
    * sees both. */
   bool host_only = !com->base.privdata_sole;
   struct npt_privdata_entry *entry = NULL;
   if (!host_only && (data || size)) {
      /* A size without data, or data without a size, is the host's to
       * judge. */
      if (data && size)
         entry = malloc(sizeof(*entry) + size);
      if (entry) {
         entry->object_id = com->base.id;
         entry->guid = *guid;
         entry->size = size;
         memcpy(entry->data, data, size);
      } else {
         host_only = true;
      }
   }
   npt_privdata_write(com, guid, entry, host_only);
}

void
npt_privdata_set_interface(void *self, const GUID *guid)
{
   if (guid)
      npt_privdata_write(self, guid, NULL, true);
}

void
npt_privdata_set_name(void *self)
{
   npt_privdata_write(self, &npt_privdata_name_a, NULL, true);
   npt_privdata_write(self, &npt_privdata_name_w, NULL, true);
}

bool
npt_privdata_get(void *self, const GUID *guid, UINT *pDataSize,
                 void *pData, HRESULT *hr)
{
   struct npt_com_base *com = self;
   struct npt_privdata *pd = npt_privdata_of(com);
   if (!pd || !guid || !pDataSize)
      return false;

   bool answered = false;
   const struct npt_privdata_entry *entry = NULL;
   AcquireSRWLockShared(&pd->lock);
   const struct npt_privdata_guid *g = npt_privdata_find_guid_locked(pd, guid);
   if (!g) {
      /* Never set on any object: the host cannot have it either. */
      answered = !pd->guid_overflow;
   } else if (com->base.privdata_sole && !g->host_only) {
      /* No other id of this object wrote the GUID, or it would be
       * host-only: no copy here means nothing set. */
      struct npt_privdata_entry **link =
         npt_privdata_find_locked(pd, com->base.id, guid);
      entry = link ? *link : NULL;
      answered = true;
   }
   if (answered && !entry) {
      *pDataSize = 0;
      *hr = NPT_DXGI_ERROR_NOT_FOUND;
   } else if (entry) {
      if (!pData) {
         *hr = NPT_S_OK;
      } else if (*pDataSize < entry->size) {
         *hr = NPT_DXGI_ERROR_MORE_DATA;
      } else {
         memcpy(pData, entry->data, entry->size);
         *hr = NPT_S_OK;
      }
      *pDataSize = entry->size;
   }
   ReleaseSRWLockShared(&pd->lock);
   return answered;
}

void
npt_privdata_forget(void *self)
{
   struct npt_com_base *com = self;
   struct npt_privdata *pd = npt_privdata_of(com);
   if (!pd || !com->base.privdata_sole)
      return;

   AcquireSRWLockExclusive(&pd->lock);
   struct npt_privdata_entry **link =
      &pd->buckets[npt_privdata_bucket(com->base.id)];
   while (*link) {
      struct npt_privdata_entry *entry = *link;
      if (entry->object_id == com->base.id) {
         *link = entry->next;
         free(entry);
      } else {
         link = &entry->next;
      }
   }
   ReleaseSRWLockExclusive(&pd->lock);
}

void
npt_privdata_fini(struct npt_privdata *pd)
{
   /* The device-destroy drain forgot every wrapper; free defensively. */
   for (uint32_t i = 0; i < NPT_PRIVDATA_BUCKETS; i++) {
      while (pd->buckets[i]) {
         struct npt_privdata_entry *entry = pd->buckets[i];
         pd->buckets[i] = entry->next;
         free(entry);
      }
   }
}
