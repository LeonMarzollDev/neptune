/*
 * SPDX-License-Identifier: MIT
 */
#include "npt_privdata.h"

#include "npt_com.h"
#include "neptune-protocol/npt_protocol_common_cs_helpers.h"

#include <stdlib.h>
#include <string.h>

struct npt_privdata_entry {
   struct npt_privdata_entry *next;
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

static struct npt_privdata *
npt_privdata_of(struct npt_com_base *com)
{
   return com->base.device ? &com->base.device->privdata : NULL;
}

static struct npt_privdata_guid *
npt_privdata_find_guid_locked(struct npt_privdata *pd, const GUID *guid)
{
   for (uint32_t i = 0; i < pd->guid_count; i++) {
      if (npt_guid_equal(&pd->guids[i].guid, guid))
         return &pd->guids[i];
   }
   return NULL;
}

/* NULL once the table is full: the host answers that GUID. */
static struct npt_privdata_guid *
npt_privdata_note_guid_locked(struct npt_privdata *pd, const GUID *guid)
{
   struct npt_privdata_guid *g = npt_privdata_find_guid_locked(pd, guid);
   if (g || pd->guid_count == NPT_PRIVDATA_GUIDS)
      return g;
   g = &pd->guids[pd->guid_count++];
   g->guid = *guid;
   g->host_only = false;
   return g;
}

/* The link to com's copy of `guid`, or to the end of its list. */
static struct npt_privdata_entry **
npt_privdata_find_locked(struct npt_com_base *com, const GUID *guid)
{
   struct npt_privdata_entry **link = &com->base.privdata;
   while (*link && !npt_guid_equal(&(*link)->guid, guid))
      link = &(*link)->next;
   return link;
}

/* A write through `com`: `entry` replaces its copy (NULL removes it), or,
 * with `host_only`, the host alone knows the GUID from now on.  Takes
 * `entry`. */
static void
npt_privdata_write_locked(struct npt_privdata *pd, struct npt_com_base *com,
                          const GUID *guid, struct npt_privdata_entry *entry,
                          bool host_only)
{
   struct npt_privdata_guid *g = npt_privdata_note_guid_locked(pd, guid);
   struct npt_privdata_entry **link = npt_privdata_find_locked(com, guid);
   struct npt_privdata_entry *old = *link;
   if (old)
      *link = old->next;
   if (g && host_only) {
      g->host_only = true;
   } else if (g && entry && !g->host_only) {
      entry->next = com->base.privdata;
      com->base.privdata = entry;
      entry = NULL;
   }
   free(old);
   free(entry);
}

static void
npt_privdata_write(struct npt_com_base *com, const GUID *guid,
                   struct npt_privdata_entry *entry, bool host_only)
{
   struct npt_privdata *pd = npt_privdata_of(com);
   if (!pd) {
      free(entry);
      return;
   }
   AcquireSRWLockExclusive(&pd->lock);
   npt_privdata_write_locked(pd, com, guid, entry, host_only);
   ReleaseSRWLockExclusive(&pd->lock);
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

   const bool sole = com->base.privdata_sole;
   struct npt_privdata_entry *entry = NULL;
   if (sole && data && size && (entry = malloc(sizeof(*entry) + size))) {
      entry->guid = *guid;
      entry->size = size;
      memcpy(entry->data, data, size);
   }
   /* The host alone knows when another id of this object may hold the
    * GUID too, when a size comes without data or data without a size (the
    * host's to judge), or when no copy could be made. */
   const bool host_only = !sole || (!entry && (data || size));
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
   struct npt_com_base *com = self;
   struct npt_privdata *pd = npt_privdata_of(com);
   if (!pd)
      return;
   AcquireSRWLockExclusive(&pd->lock);
   npt_privdata_write_locked(pd, com, &npt_privdata_name_a, NULL, true);
   npt_privdata_write_locked(pd, com, &npt_privdata_name_w, NULL, true);
   ReleaseSRWLockExclusive(&pd->lock);
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
      /* Missing from a table with room: never set on any object, so the
       * host cannot have it either.  A full table may have missed it. */
      answered = pd->guid_count < NPT_PRIVDATA_GUIDS;
   } else if (com->base.privdata_sole && !g->host_only) {
      /* No other id of this object wrote the GUID, or it would be
       * host-only: no copy here means nothing set. */
      entry = *npt_privdata_find_locked(com, guid);
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
npt_privdata_free(void *self)
{
   /* The wrapper is being destroyed: no other thread reaches it. */
   struct npt_com_base *com = self;
   while (com->base.privdata) {
      struct npt_privdata_entry *entry = com->base.privdata;
      com->base.privdata = entry->next;
      free(entry);
   }
}
