/*
 * SPDX-License-Identifier: MIT
 *
 * Guest-side record of ID3D12Object private data.
 *
 * ID3D12Object::GetPrivateData is a synchronous round trip to the render
 * server, and some engines and middleware poll it (Bodycam: about 1,250
 * calls a second, 290 us each).  Everything the host knows was set by the
 * guest, so the guest keeps a copy and answers from it.  A lookup falls back
 * to the host whenever the local record cannot be sure of the answer.
 */
#ifndef NPT_PRIVDATA_H
#define NPT_PRIVDATA_H

#include "npt_common.h"
#include "npt_cs.h"

/* Record SetPrivateData (data == NULL or size == 0 removes the entry). */
void npt_privdata_set(uint64_t object_id, const GUID *guid, UINT size,
                      const void *data);

/* Record that the host holds data for (object, guid) the guest has no copy
 * of: SetPrivateDataInterface and SetName. */
void npt_privdata_set_opaque(uint64_t object_id, const GUID *guid);
void npt_privdata_note_name(uint64_t object_id);

/* true: *hr and the outputs hold the answer.  false: ask the host. */
bool npt_privdata_get(uint64_t object_id, const GUID *guid, UINT *pDataSize,
                      void *pData, HRESULT *hr);

/* Drop every entry of a destroyed object. */
void npt_privdata_forget(uint64_t object_id);

#endif
