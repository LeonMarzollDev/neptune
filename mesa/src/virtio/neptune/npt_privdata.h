/*
 * SPDX-License-Identifier: MIT
 *
 * Guest-side record of ID3D12Object private data.
 *
 * ID3D12Object::GetPrivateData is a synchronous round trip to the render
 * server, and some engines poll it: Unreal Engine 5's residency manager
 * (D3DX12Residency) reads its fence from the command queue's private data
 * at every ExecuteCommandLists (Bodycam: about 1,250 calls a second, 290 us
 * each).  Everything the host knows was set by the guest, so the guest
 * keeps a copy and answers from it when the host's answer cannot differ.
 *
 * The host keeps private data per object, but one host object can have
 * several guest ids: GetDevice and a QueryInterface the wrapper does not
 * answer itself hand out a new id, D3D11On12 wraps an unwrapped resource
 * again.  Data set through one id is read through the others, so a copy
 * kept per id would go stale.  The guest keeps copies only for wrappers
 * marked as the sole id of their host object (npt_privdata_mark_sole), and
 * a GUID written through any other id is answered by the host from then on,
 * for every object.
 *
 * A copy lives on its wrapper (npt_object.privdata) and is freed with it.
 * The GUIDs set and their host-only marks live in struct npt_device, which
 * d3d11.dll, dxgi.dll and d3d12.dll share: a GUID set through one DLL is
 * known to the others.
 */
#ifndef NPT_PRIVDATA_H
#define NPT_PRIVDATA_H

#include "npt_common.h"
#include "npt_cs.h"

#define NPT_PRIVDATA_GUIDS 256u

struct npt_privdata_guid {
   GUID guid;
   /* Written through an id the guest keeps no copy for, given an
    * interface or a name: only the host knows. */
   bool host_only;
};

/* Zero-initialized is empty. */
struct npt_privdata {
   /* Guards the GUID table and every wrapper's copy. */
   SRWLOCK lock;
   /* Every GUID written through any id.  A GUID missing here was never
    * set anywhere, so NOT_FOUND is certain without asking the host.
    * Once the table is full, lookups of a GUID missing from it go to the
    * host. */
   struct npt_privdata_guid guids[NPT_PRIVDATA_GUIDS];
   uint32_t guid_count;
};

/* Mark a wrapper as the sole guest id of its host object, so its data is
 * answered from the guest's copy.  The guest relies on at most one marked
 * id per host object: mark a wrapper just made for a new host object, and
 * leave every later handout of it unmarked (a QueryInterface the wrapper
 * does not answer itself).  A write through an unmarked id makes the GUID
 * host-only. */
void npt_privdata_mark_sole(void *self);

/* Record SetPrivateData (data == NULL and size == 0 removes the entry),
 * SetPrivateDataInterface and SetName. */
void npt_privdata_set(void *self, const GUID *guid, UINT size,
                      const void *data);
void npt_privdata_set_interface(void *self, const GUID *guid);
void npt_privdata_set_name(void *self);

/* true: *hr and the outputs hold the answer.  false: ask the host. */
bool npt_privdata_get(void *self, const GUID *guid, UINT *pDataSize,
                      void *pData, HRESULT *hr);

/* Free the copy of a wrapper being destroyed. */
void npt_privdata_free(void *self);

#endif
