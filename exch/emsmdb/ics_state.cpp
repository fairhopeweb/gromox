// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <memory>
#include "common_util.h"
#include "ics_state.h"
#include <gromox/rop_util.hpp>
#include <gromox/idset.hpp>
#include <cstdlib>
#include <cstring>

ICS_STATE::~ICS_STATE()
{
	auto pstate = this;
	if (NULL != pstate->pgiven) {
		idset_free(pstate->pgiven);
		pstate->pgiven = NULL;
	}
	if (NULL != pstate->pseen) {
		idset_free(pstate->pseen);
		pstate->pseen = NULL;
	}
	if (NULL != pstate->pseen_fai) {
		idset_free(pstate->pseen_fai);
		pstate->pseen_fai = NULL;
	}
	if (NULL != pstate->pread) {
		idset_free(pstate->pread);
		pstate->pread = NULL;
	}
}

std::unique_ptr<ics_state> ics_state::create(logon_object *plogon, int type)
{
	std::unique_ptr<ICS_STATE> pstate;
	BINARY tmp_bin;
	try {
		pstate.reset(new ics_state);
	} catch (const std::bad_alloc &) {
		return NULL;
	}
	tmp_bin.cb = sizeof(void*);
	tmp_bin.pv = &plogon;
	pstate->pseen = idset_init(TRUE, REPL_TYPE_GUID);
	if (NULL == pstate->pseen) {
		return NULL;
	}
	if (!pstate->pseen->register_mapping(&tmp_bin, common_util_mapping_replica))
		return NULL;
	switch (type) {
	case ICS_STATE_CONTENTS_DOWN:
		pstate->pgiven = idset_init(TRUE, REPL_TYPE_GUID);
		if (NULL == pstate->pgiven) {
			return NULL;
		}
		if (!pstate->pgiven->register_mapping(&tmp_bin,
		    common_util_mapping_replica))
			return NULL;
		pstate->pseen_fai = idset_init(TRUE, REPL_TYPE_GUID);
		if (NULL == pstate->pseen_fai) {
			return NULL;
		}
		if (!pstate->pseen_fai->register_mapping(&tmp_bin,
		    common_util_mapping_replica))
			return NULL;
		pstate->pread = idset_init(TRUE, REPL_TYPE_GUID);
		if (NULL == pstate->pread) {
			return NULL;
		}
		if (!pstate->pread->register_mapping(&tmp_bin,
		    common_util_mapping_replica))
			return NULL;
		break;
	case ICS_STATE_HIERARCHY_DOWN:
		pstate->pgiven = idset_init(TRUE, REPL_TYPE_GUID);
		if (NULL == pstate->pgiven) {
			return NULL;
		}
		if (!pstate->pgiven->register_mapping(&tmp_bin,
		    common_util_mapping_replica))
			return NULL;
		break;
	case ICS_STATE_CONTENTS_UP:
		pstate->pgiven = idset_init(TRUE, REPL_TYPE_GUID);
		if (NULL == pstate->pgiven) {
			return NULL;
		}
		if (!pstate->pgiven->register_mapping(&tmp_bin,
		    common_util_mapping_replica))
			return NULL;
		pstate->pseen_fai = idset_init(TRUE, REPL_TYPE_GUID);
		if (NULL == pstate->pseen_fai) {
			return NULL;
		}
		if (!pstate->pseen_fai->register_mapping(&tmp_bin,
		    common_util_mapping_replica))
			return NULL;
		pstate->pread = idset_init(TRUE, REPL_TYPE_GUID);
		if (NULL == pstate->pread) {
			return NULL;
		}
		if (!pstate->pread->register_mapping(&tmp_bin,
		    common_util_mapping_replica))
			return NULL;
		break;
	case ICS_STATE_HIERARCHY_UP:
		break;
	}
	pstate->type = type;
	return pstate;
}

BOOL ICS_STATE::append_idset(uint32_t state_property, IDSET *pset)
{
	auto pstate = this;
	switch (state_property) {
	case META_TAG_IDSETGIVEN:
	case META_TAG_IDSETGIVEN1:
		if (NULL != pstate->pgiven) {
			idset_free(pstate->pgiven);
		}
		pstate->pgiven = pset;
		return TRUE;
	case META_TAG_CNSETSEEN:
		if (NULL != pstate->pseen) {
			if ((ICS_STATE_CONTENTS_UP == pstate->type ||
				ICS_STATE_HIERARCHY_UP == pstate->type) &&
			    !pstate->pseen->check_empty() &&
			    !pset->concatenate(pstate->pseen))
				return FALSE;
			idset_free(pstate->pseen);
		}
		pstate->pseen = pset;
		return TRUE;
	case META_TAG_CNSETSEENFAI:
		if (NULL != pstate->pseen_fai) {
			if (ICS_STATE_CONTENTS_UP == pstate->type &&
			    !pstate->pseen_fai->check_empty() &&
			    !pset->concatenate(pstate->pseen_fai))
				return FALSE;
			idset_free(pstate->pseen_fai);
		}
		pstate->pseen_fai = pset;
		return TRUE;
	case META_TAG_CNSETREAD:
		if (NULL != pstate->pread) {
			if (ICS_STATE_CONTENTS_UP == pstate->type &&
			    !pstate->pread->check_empty() &&
			    !pset->concatenate(pstate->pread))
				return FALSE;
			idset_free(pstate->pread);
		}
		pstate->pread = pset;
		return TRUE;
	}
	return FALSE;
}

TPROPVAL_ARRAY *ICS_STATE::serialize()
{
	auto pstate = this;
	TPROPVAL_ARRAY *pproplist;
	
	
	pproplist = tpropval_array_init();
	if (NULL == pproplist) {
		return NULL;
	}
	
	if (ICS_STATE_CONTENTS_DOWN == pstate->type ||
		ICS_STATE_HIERARCHY_DOWN == pstate->type ||
		(ICS_STATE_CONTENTS_UP == pstate->type &&
	    !pstate->pgiven->check_empty())) {
		auto pbin = pstate->pgiven->serialize();
		if (NULL == pbin) {
			tpropval_array_free(pproplist);
			return NULL;
		}
		if (pproplist->set(META_TAG_IDSETGIVEN1, pbin) != 0) {
			rop_util_free_binary(pbin);
			tpropval_array_free(pproplist);
			return NULL;
		}
		rop_util_free_binary(pbin);
	}
	
	auto pbin = pstate->pseen->serialize();
	if (NULL == pbin) {
		tpropval_array_free(pproplist);
		return NULL;
	}
	if (pproplist->set(META_TAG_CNSETSEEN, pbin) != 0) {
		rop_util_free_binary(pbin);
		tpropval_array_free(pproplist);
		return NULL;
	}
	rop_util_free_binary(pbin);
	
	if (ICS_STATE_CONTENTS_DOWN == pstate->type ||
		ICS_STATE_CONTENTS_UP == pstate->type) {
		pbin = pstate->pseen_fai->serialize();
		if (NULL == pbin) {
			tpropval_array_free(pproplist);
			return NULL;
		}
		if (pproplist->set(META_TAG_CNSETSEENFAI, pbin) != 0) {
			rop_util_free_binary(pbin);
			tpropval_array_free(pproplist);
			return NULL;
		}
		rop_util_free_binary(pbin);
	}
	
	if (ICS_STATE_CONTENTS_DOWN == pstate->type ||
		(ICS_STATE_CONTENTS_UP == pstate->type &&
	    !pstate->pread->check_empty())) {
		pbin = pstate->pread->serialize();
		if (NULL == pbin) {
			tpropval_array_free(pproplist);
			return NULL;
		}
		if (pproplist->set(META_TAG_CNSETREAD, pbin) != 0) {
			rop_util_free_binary(pbin);
			tpropval_array_free(pproplist);
			return NULL;
		}
		rop_util_free_binary(pbin);
	}
	
	return pproplist;
}
