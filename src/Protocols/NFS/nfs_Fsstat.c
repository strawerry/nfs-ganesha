/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * ---------------------------------------
 */

/**
 * \file    nfs_Fsstat.c
 * \author  $Author: deniel $
 * \date    $Date: 2005/11/28 17:02:53 $
 * \version $Revision: 1.14 $
 * \brief   Routines used for managing the NFS4 COMPOUND functions.
 *
 * nfs_Fsstat.c : Routines used for managing the NFS4 COMPOUND functions.
 *
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _SOLARIS
#include "solaris_port.h"
#endif

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/file.h>           /* for having FNDELAY */
#include "HashData.h"
#include "HashTable.h"
#include "log.h"
#include "ganesha_rpc.h"
#include "nfs23.h"
#include "nfs4.h"
#include "mount.h"
#include "nfs_core.h"
#include "cache_inode.h"
#include "nfs_exports.h"
#include "nfs_creds.h"
#include "nfs_proto_functions.h"
#include "nfs_tools.h"
#include "nfs_proto_tools.h"

/**
 *
 * @brief The NFS PROC2 and PROC3 FSSTAT
 *
 * Implements the NFS PROC2 and PROC3 FSSTAT.
 *
 * @param[in]  parg     NFS argument union
 * @param[in]  pexport  NFS export list
 * @param[in]  pcontext Credentials to be used for this request
 * @param[in]  pworker  Worker thread data
 * @param[in]  preq     SVC request related to this call
 * @param[out] pres     Structure to contain the result of the call
 *
 * @retval NFS_REQ_OK if successful
 * @retval NFS_REQ_DROP if failed but retryable
 * @retval NFS_REQ_FAILED if failed and not retryable
 *
 */

int nfs_Fsstat(nfs_arg_t *parg,
               exportlist_t *pexport,
               fsal_op_context_t *pcontext,
               nfs_worker_data_t *pworker,
               struct svc_req *preq,
               nfs_res_t * pres)
{
  fsal_dynamicfsinfo_t dynamicinfo;
  cache_inode_status_t cache_status;
  cache_entry_t *pentry = NULL;
  fsal_attrib_list_t attr;
  int rc = NFS_REQ_OK;

  if(isDebug(COMPONENT_NFSPROTO))
    {
      char                  str[LEN_FH_STR];
      struct display_buffer dspbuf = {sizeof(str), str, str};

      if(preq->rq_vers == NFS_V2)
        (void) display_fhandle2(&dspbuf, (fhandle2 *) parg);
      else
        (void) display_fhandle3(&dspbuf, (nfs_fh3 *) parg);

      LogDebug(COMPONENT_NFSPROTO,
               "REQUEST PROCESSING: Calling nfs_Fsstat handle: %s", str);
    }

  if(preq->rq_vers == NFS_V3)
    {
      /* to avoid setting it on each error case */
      pres->res_fsstat3.FSSTAT3res_u.resfail.obj_attributes.attributes_follow = FALSE;
    }

  /* convert file handle to vnode */
  if((pentry = nfs_FhandleToCache(preq->rq_vers,
                                  &(parg->arg_statfs2),
                                  &(parg->arg_fsstat3.fsroot),
                                  NULL,
                                  &(pres->res_statfs2.status),
                                  &(pres->res_fsstat3.status),
                                  NULL, NULL, pcontext, &rc)) == NULL)
    {
      /* Stale NFS FH ? */
      /* return NFS_REQ_DROP ; */
      goto out;
    }

  /* Get statistics and convert from cache */

  if((cache_status = cache_inode_statfs(pentry,
                                        &dynamicinfo,
                                        pcontext, &cache_status)) == CACHE_INODE_SUCCESS)
    {
      /* This call is costless, the pentry was cached during call to nfs_FhandleToCache */
      if((cache_status = cache_inode_getattr(pentry,
                                             &attr,
                                             pcontext,
                                             &cache_status)) == CACHE_INODE_SUCCESS)
        {

          LogFullDebug(COMPONENT_NFSPROTO,
                       "nfs_Fsstat --> dynamicinfo.total_bytes = %zu dynamicinfo.free_bytes = %zu dynamicinfo.avail_bytes = %zu",
                       dynamicinfo.total_bytes,
                       dynamicinfo.free_bytes,
                       dynamicinfo.avail_bytes);
          LogFullDebug(COMPONENT_NFSPROTO, 
                       "nfs_Fsstat --> dynamicinfo.total_files = %llu dynamicinfo.free_files = %llu dynamicinfo.avail_files = %llu",
                       dynamicinfo.total_files,
                       dynamicinfo.free_files,
                       dynamicinfo.avail_files);

          switch (preq->rq_vers)
            {
            case NFS_V2:
              pres->res_statfs2.STATFS2res_u.info.tsize = NFS2_MAXDATA;
              pres->res_statfs2.STATFS2res_u.info.bsize = DEV_BSIZE;
              pres->res_statfs2.STATFS2res_u.info.blocks =
                  dynamicinfo.total_bytes / DEV_BSIZE;
              pres->res_statfs2.STATFS2res_u.info.bfree =
                  dynamicinfo.free_bytes / DEV_BSIZE;
              pres->res_statfs2.STATFS2res_u.info.bavail =
                  dynamicinfo.avail_bytes / DEV_BSIZE;
              pres->res_statfs2.status = NFS_OK;
              break;

            case NFS_V3:
              nfs_SetPostOpAttr(pexport,
                                &attr,
                                &(pres->res_fsstat3.FSSTAT3res_u
                                  .resok.obj_attributes));

              pres->res_fsstat3.FSSTAT3res_u.resok.tbytes = dynamicinfo.total_bytes;
              pres->res_fsstat3.FSSTAT3res_u.resok.fbytes = dynamicinfo.free_bytes;
              pres->res_fsstat3.FSSTAT3res_u.resok.abytes = dynamicinfo.avail_bytes;
              pres->res_fsstat3.FSSTAT3res_u.resok.tfiles = dynamicinfo.total_files;
              pres->res_fsstat3.FSSTAT3res_u.resok.ffiles = dynamicinfo.free_files;
              pres->res_fsstat3.FSSTAT3res_u.resok.afiles = dynamicinfo.avail_files;
              pres->res_fsstat3.FSSTAT3res_u.resok.invarsec = 0;        /* volatile FS */
              pres->res_fsstat3.status = NFS3_OK;

              LogFullDebug(COMPONENT_NFSPROTO,
                           "nfs_Fsstat --> tbytes=%llu fbytes=%llu abytes=%llu",
                           pres->res_fsstat3.FSSTAT3res_u.resok.tbytes,
                           pres->res_fsstat3.FSSTAT3res_u.resok.fbytes,
                           pres->res_fsstat3.FSSTAT3res_u.resok.abytes);

	      LogFullDebug(COMPONENT_NFSPROTO,
	                   "nfs_Fsstat --> tfiles=%llu fffiles=%llu afiles=%llu",
                           pres->res_fsstat3.FSSTAT3res_u.resok.tfiles,
                           pres->res_fsstat3.FSSTAT3res_u.resok.ffiles,
                           pres->res_fsstat3.FSSTAT3res_u.resok.afiles);

              break;

            }
          rc = NFS_REQ_OK;
          goto out;
        }
    }

  /* At this point we met an error */
  rc = nfs_SetFailedStatus(pexport, preq->rq_vers, cache_status,
                           &pres->res_statfs2.status,
                           &pres->res_fsstat3.status,
                           NULL, NULL, NULL, NULL, NULL);
out:
  /* return references */
  if (pentry)
      cache_inode_put(pentry);

  return (rc);

}                               /* nfs_Fsstat */

/**
 * nfs_Fsstat_Free: Frees the result structure allocated for nfs_Fsstat.
 * 
 * Frees the result structure allocated for nfs_Fsstat.
 * 
 * @param pres        [INOUT]   Pointer to the result structure.
 *
 */
void nfs_Fsstat_Free(nfs_res_t * resp)
{
  /* Nothing to do here */
  return;
}                               /* nfs_Fsstat_Free */