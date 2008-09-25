/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 *  (C) 2006 by Argonne National Laboratory.
 *      See COPYRIGHT in top-level directory.
 */

#include "mpid_nem_pre.h"
#include "mpid_nem_impl.h"
#include "mpid_nem_nets.h"
#include <errno.h>

#ifdef MEM_REGION_IN_HEAP
MPID_nem_mem_region_t *MPID_nem_mem_region_ptr = 0;
#else /* MEM_REGION_IN_HEAP */
MPID_nem_mem_region_t MPID_nem_mem_region = {{0}};
#endif /* MEM_REGION_IN_HEAP */

char MPID_nem_hostname[MAX_HOSTNAME_LEN] = "UNKNOWN";

static MPID_nem_queue_ptr_t net_free_queue;

#define MIN( a , b ) ((a) >  (b)) ? (b) : (a)
#define MAX( a , b ) ((a) >= (b)) ? (a) : (b)

char *MPID_nem_asymm_base_addr = 0;

static int get_local_procs (int rank, int num_procs, int *num_local, int **local_procs, int *local_rank, int *num_nodes, int **node_ids);
static int get_local_procs_nolocal(int global_rank, int num_global, int *num_local_p, int **local_procs_p, int *local_rank_p, int *num_nodes_p, int **node_ids_p);

int
MPID_nem_init (int rank, MPIDI_PG_t *pg_p, int has_parent)
{
    return  _MPID_nem_init (rank, pg_p, 0, has_parent);
}

#undef FUNCNAME
#define FUNCNAME MPID_nem_init
#undef FCNAME
#define FCNAME MPIDI_QUOTE(FUNCNAME)
int
_MPID_nem_init (int pg_rank, MPIDI_PG_t *pg_p, int ckpt_restart, int has_parent)
{
    int    mpi_errno       = MPI_SUCCESS;
    int    pmi_errno;
    int    num_procs       = pg_p->size;
    pid_t  my_pid;
    int    ret;
    int    num_local       = -1;
    int   *local_procs     = NULL;
    int    local_rank      = -1;
    int    index;
    int    i;
    char  *publish_bc_orig = NULL;
    char  *bc_val          = NULL;
    int    val_max_remaining;
    int    num_nodes = 0;
    int   *node_ids = 0;
    int    grank;
    MPID_nem_fastbox_t *fastboxes_p = NULL;
    MPID_nem_cell_t (*cells_p)[MPID_NEM_NUM_CELLS];
    MPID_nem_cell_t (*network_cells_p)[MPID_NEM_NUM_CELLS];
    MPID_nem_queue_t *recv_queues_p = NULL;
    MPID_nem_queue_t *free_queues_p = NULL;
    MPID_nem_barrier_t *barrier_region_p = NULL;
    pid_t *pids_p = NULL;
    

    MPIU_CHKPMEM_DECL(9);

    /* Make sure the nemesis packet is no larger than the generic
       packet.  This is needed because we no longer include channel
       packet types in the CH3 packet types to allow dynamic channel
       loading. */
    MPIU_Assert(sizeof(MPIDI_CH3_nem_pkt_t) <= sizeof(MPIDI_CH3_PktGeneric_t));

    /* Make sure the cell structure looks like it should */
    MPIU_Assert(MPID_NEM_CELL_PAYLOAD_LEN + MPID_NEM_CELL_HEAD_LEN == sizeof(MPID_nem_cell_t));
    MPIU_Assert(sizeof(MPID_nem_cell_t) == sizeof(MPID_nem_abs_cell_t));
    /* Make sure payload is aligned on a double */
    MPIU_Assert(MPID_NEM_ALIGNED(&((MPID_nem_cell_t*)0)->pkt.mpich2.payload[0], sizeof(double)));


    /* Initialize the business card */
    mpi_errno = MPIDI_CH3I_BCInit( &bc_val, &val_max_remaining );
    if (mpi_errno) MPIU_ERR_POP (mpi_errno);
    publish_bc_orig = bc_val;

    ret = gethostname (MPID_nem_hostname, MAX_HOSTNAME_LEN);
    MPIU_ERR_CHKANDJUMP2 (ret == -1, mpi_errno, MPI_ERR_OTHER, "**sock_gethost", "**sock_gethost %s %d", strerror (errno), errno);

    MPID_nem_hostname[MAX_HOSTNAME_LEN-1] = '\0';

    mpi_errno = get_local_procs (pg_rank, num_procs, &num_local, &local_procs, &local_rank, &num_nodes, &node_ids);
    if (mpi_errno) MPIU_ERR_POP (mpi_errno);

#ifdef MEM_REGION_IN_HEAP
    MPIU_CHKPMEM_MALLOC (MPID_nem_mem_region_ptr, MPID_nem_mem_region_t *, sizeof(MPID_nem_mem_region_t), mpi_errno, "mem_region");
#endif /* MEM_REGION_IN_HEAP */

    MPID_nem_mem_region.num_seg        = 7;
    MPIU_CHKPMEM_MALLOC (MPID_nem_mem_region.seg, MPID_nem_seg_info_ptr_t, MPID_nem_mem_region.num_seg * sizeof(MPID_nem_seg_info_t), mpi_errno, "mem_region segments");
    MPIU_CHKPMEM_MALLOC (MPID_nem_mem_region.pid, pid_t *, num_local * sizeof(pid_t), mpi_errno, "mem_region pid list");
    MPID_nem_mem_region.rank           = pg_rank;
    MPID_nem_mem_region.num_nodes      = num_nodes;
    MPID_nem_mem_region.node_ids       = node_ids;
    MPID_nem_mem_region.num_local      = num_local;
    MPID_nem_mem_region.num_procs      = num_procs;
    MPID_nem_mem_region.local_procs    = local_procs;
    MPID_nem_mem_region.local_rank     = local_rank;
    MPIU_CHKPMEM_MALLOC (MPID_nem_mem_region.local_ranks, int *, num_procs * sizeof(int), mpi_errno, "mem_region local ranks");
    MPID_nem_mem_region.ext_procs      = num_procs - num_local ;
    MPIU_CHKPMEM_MALLOC (MPID_nem_mem_region.ext_ranks, int *, MPID_nem_mem_region.ext_procs * sizeof(int), mpi_errno, "mem_region ext ranks");
    MPID_nem_mem_region.next           = NULL;

    for (index = 0 ; index < num_procs; index++)
    {
	MPID_nem_mem_region.local_ranks[index] = MPID_NEM_NON_LOCAL;
    }
    for (index = 0; index < num_local; index++)
    {
	grank = local_procs[index];
	MPID_nem_mem_region.local_ranks[grank] = index;
    }

    index = 0;
    for(grank = 0 ; grank < num_procs ; grank++)
    {
	if(!MPID_NEM_IS_LOCAL(grank))
	{
	    MPID_nem_mem_region.ext_ranks[index++] = grank;
	}
    }

#ifdef FORCE_ASYM
    {
        /* this is used for debugging
           each process allocates a different sized piece of shared
           memory so that when the shared memory segment used for
           communication is allocated it will probably be mapped at a
           different location for each process
        */
        char *handle;
	int size = (local_rank * 65536) + 65536;
	char *base_addr;

        mpi_errno = MPID_nem_allocate_shared_memory (&base_addr, size, &handle);
        /* --BEGIN ERROR HANDLING-- */
        if (mpi_errno)
        {
            MPID_nem_remove_shared_memory (handle);
            MPIU_Free (handle);
            MPIU_ERR_POP (mpi_errno);
        }
        /* --END ERROR HANDLING-- */

        mpi_errno = MPID_nem_remove_shared_memory (handle);
        /* --BEGIN ERROR HANDLING-- */
        if (mpi_errno)
        {
            MPIU_Free (handle);
            MPIU_ERR_POP (mpi_errno);
        }
        /* --END ERROR HANDLING-- */

        MPIU_Free (handle);
    }
    /*fprintf(stderr,"[%i] -- address shift ok \n",pg_rank); */
#endif  /*FORCE_ASYM */

    /* Request fastboxes region */
    mpi_errno = MPIDI_CH3I_Seg_alloc(MAX((num_local*((num_local-1)*sizeof(MPID_nem_fastbox_t))), MPID_NEM_ASYMM_NULL_VAL),
                                     (void **)&fastboxes_p);
    if (mpi_errno) MPIU_ERR_POP(mpi_errno);
    
    /* Request data cells region */
    mpi_errno = MPIDI_CH3I_Seg_alloc(num_local * MPID_NEM_NUM_CELLS * sizeof(MPID_nem_cell_t), (void **)&cells_p);
    if (mpi_errno) MPIU_ERR_POP(mpi_errno);

    /* Request network data cells region */
    mpi_errno = MPIDI_CH3I_Seg_alloc(num_local  * MPID_NEM_NUM_CELLS * sizeof(MPID_nem_cell_t), (void **)&network_cells_p);
    if (mpi_errno) MPIU_ERR_POP(mpi_errno);

    /* Request free q region */
    mpi_errno = MPIDI_CH3I_Seg_alloc(num_local * sizeof(MPID_nem_queue_t), (void **)&free_queues_p);
    if (mpi_errno) MPIU_ERR_POP(mpi_errno);

    /* Request recv q region */
    mpi_errno = MPIDI_CH3I_Seg_alloc(num_local * sizeof(MPID_nem_queue_t), (void **)&recv_queues_p);
    if (mpi_errno) MPIU_ERR_POP(mpi_errno);

    /* Request local process barrier data region */
    mpi_errno = MPIDI_CH3I_Seg_alloc(sizeof(MPID_nem_barrier_t), (void **)&barrier_region_p);
    if (mpi_errno) MPIU_ERR_POP(mpi_errno);

    /* Request shared collectives barrier vars region */
    mpi_errno = MPIDI_CH3I_Seg_alloc(MPID_NEM_NUM_BARRIER_VARS * sizeof(MPID_nem_barrier_vars_t),
                                     (void **)&MPID_nem_mem_region.barrier_vars);
    if (mpi_errno) MPIU_ERR_POP(mpi_errno);

    /* Request pids region */
    mpi_errno = MPIDI_CH3I_Seg_alloc(num_local * sizeof(pid_t), (void **)&pids_p);
    if (mpi_errno) MPIU_ERR_POP(mpi_errno);

    
    /* Actually allocate the segment and assign regions to the pointers */
    mpi_errno = MPIDI_CH3I_Seg_commit(&MPID_nem_mem_region.memory, num_local, local_rank);
    if (mpi_errno) MPIU_ERR_POP (mpi_errno);

    
    /* init shared collectives barrier region */
    mpi_errno = MPID_nem_barrier_vars_init(MPID_nem_mem_region.barrier_vars);
    if (mpi_errno) MPIU_ERR_POP (mpi_errno);

    /* set up local process barrier region */
    mpi_errno = MPID_nem_barrier_init(barrier_region_p);
    if (mpi_errno) MPIU_ERR_POP (mpi_errno);

    /* global barrier */
    pmi_errno = PMI_Barrier();
    MPIU_ERR_CHKANDJUMP1 (pmi_errno != PMI_SUCCESS, mpi_errno, MPI_ERR_OTHER, "**pmi_barrier", "**pmi_barrier %d", pmi_errno);

    
    /* exchange PIDs */
    my_pid = getpid();
    pids_p[local_rank] = my_pid;

    /* local procs barrier */
    mpi_errno = MPID_nem_barrier(num_local, local_rank);
    if (mpi_errno) MPIU_ERR_POP (mpi_errno);

    /* read pids */
    for (index = 0 ; index < num_local ; index ++)
    {
	MPID_nem_mem_region.pid[index] = pids_p[index];
    }

    /* local procs barrier */
    mpi_errno = MPID_nem_barrier (num_local, local_rank);
    if (mpi_errno) MPIU_ERR_POP (mpi_errno);

    
    /* find our cell regions */
    MPID_nem_mem_region.Elements = cells_p[local_rank];
    MPID_nem_mem_region.net_elements = network_cells_p[local_rank];

    /* Tables of pointers to shared memory Qs */
    MPIU_CHKPMEM_MALLOC(MPID_nem_mem_region.FreeQ, MPID_nem_queue_ptr_t *, num_procs * sizeof(MPID_nem_queue_ptr_t), mpi_errno, "FreeQ");
    MPIU_CHKPMEM_MALLOC(MPID_nem_mem_region.RecvQ, MPID_nem_queue_ptr_t *, num_procs * sizeof(MPID_nem_queue_ptr_t), mpi_errno, "RecvQ");

    /* Init table entry for our Qs */
    MPID_nem_mem_region.FreeQ[pg_rank] = &free_queues_p[local_rank];
    MPID_nem_mem_region.RecvQ[pg_rank] = &recv_queues_p[local_rank];

    /* Init our queues */
    MPID_nem_queue_init(MPID_nem_mem_region.RecvQ[pg_rank]);
    MPID_nem_queue_init(MPID_nem_mem_region.FreeQ[pg_rank]);
    
    /* Init and enqueue our free cells */
    for (index = 0; index < MPID_NEM_NUM_CELLS; ++index)
    {
	MPID_nem_cell_init(&(MPID_nem_mem_region.Elements[index]));
	MPID_nem_queue_enqueue(MPID_nem_mem_region.FreeQ[pg_rank], &(MPID_nem_mem_region.Elements[index]));
    }

    /* network init */
    if (MPID_nem_num_netmods)
    {
        mpi_errno = MPID_nem_choose_netmod();
        if (mpi_errno) MPIU_ERR_POP(mpi_errno);
	mpi_errno = MPID_nem_netmod_func->init(MPID_nem_mem_region.RecvQ[pg_rank],
                                               MPID_nem_mem_region.FreeQ[pg_rank],
                                               MPID_nem_mem_region.Elements,
                                               MPID_NEM_NUM_CELLS,
                                               MPID_nem_mem_region.net_elements,
                                               MPID_NEM_NUM_CELLS,
                                               &net_free_queue,
                                               ckpt_restart, pg_p, pg_rank,
                                               &bc_val, &val_max_remaining);
        if (mpi_errno) MPIU_ERR_POP(mpi_errno);
    }
    else
    {
        net_free_queue = NULL;
    }

    /* set default route for external processes through network */
    for (index = 0 ; index < MPID_nem_mem_region.ext_procs ; index++)
    {
	grank = MPID_nem_mem_region.ext_ranks[index];
	MPID_nem_mem_region.FreeQ[grank] = net_free_queue;
	MPID_nem_mem_region.RecvQ[grank] = NULL;
    }


    /* set route for local procs through shmem */
    for (index = 0; index < num_local; index++)
    {
	grank = local_procs[index];
	MPID_nem_mem_region.FreeQ[grank] = &free_queues_p[index];
	MPID_nem_mem_region.RecvQ[grank] = &recv_queues_p[index];
        
	MPIU_Assert(MPID_NEM_ALIGNED(MPID_nem_mem_region.FreeQ[grank], MPID_NEM_CACHE_LINE_LEN));
	MPIU_Assert(MPID_NEM_ALIGNED(MPID_nem_mem_region.RecvQ[grank], MPID_NEM_CACHE_LINE_LEN));
    }

    /* make pointers to our queues global so we don't have to dereference the array */
    MPID_nem_mem_region.my_freeQ = MPID_nem_mem_region.FreeQ[pg_rank];
    MPID_nem_mem_region.my_recvQ = MPID_nem_mem_region.RecvQ[pg_rank];

    
    /* local barrier */
    mpi_errno = MPID_nem_barrier(num_local, local_rank);
    if (mpi_errno) MPIU_ERR_POP(mpi_errno);

    
    /* Allocate table of pointers to fastboxes */
    MPIU_CHKPMEM_MALLOC(MPID_nem_mem_region.mailboxes.in,  MPID_nem_fastbox_t **, num_local * sizeof(MPID_nem_fastbox_t *), mpi_errno, "fastboxes");
    MPIU_CHKPMEM_MALLOC(MPID_nem_mem_region.mailboxes.out, MPID_nem_fastbox_t **, num_local * sizeof(MPID_nem_fastbox_t *), mpi_errno, "fastboxes");

    MPIU_Assert(num_local > 0);

#define MAILBOX_INDEX(sender, receiver) ( ((sender) > (receiver)) ? ((num_local-1) * (sender) + (receiver)) :		\
                                          (((sender) < (receiver)) ? ((num_local-1) * (sender) + ((receiver)-1)) : 0) )

    /* fill in tables */
    for (i = 0; i < num_local; ++i)
    {
	if (i == local_rank)
	{
            /* No fastboxs to myself */
	    MPID_nem_mem_region.mailboxes.in [i] = NULL ;
	    MPID_nem_mem_region.mailboxes.out[i] = NULL ;
	}
	else
	{
	    MPID_nem_mem_region.mailboxes.in [i] = &fastboxes_p[MAILBOX_INDEX(i, local_rank)];
	    MPID_nem_mem_region.mailboxes.out[i] = &fastboxes_p[MAILBOX_INDEX(local_rank, i)];
	    MPID_nem_mem_region.mailboxes.in [i]->common.flag.value = 0;
	    MPID_nem_mem_region.mailboxes.out[i]->common.flag.value = 0;
	}
    }
#undef MAILBOX_INDEX

    /* publish business card */
    mpi_errno = MPIDI_PG_SetConnInfo(pg_rank, (const char *)publish_bc_orig);
    if (mpi_errno) MPIU_ERR_POP(mpi_errno);
    MPIU_Free(publish_bc_orig);


    mpi_errno = MPID_nem_barrier(num_local, local_rank);
    if (mpi_errno) MPIU_ERR_POP(mpi_errno);
    mpi_errno = MPID_nem_mpich2_init(ckpt_restart);
    if (mpi_errno) MPIU_ERR_POP(mpi_errno);
    mpi_errno = MPID_nem_barrier(num_local, local_rank);
    if (mpi_errno) MPIU_ERR_POP(mpi_errno);

#ifdef ENABLED_CHECKPOINTING
    MPID_nem_ckpt_init(ckpt_restart);
#endif


#ifdef PAPI_MONITOR
    my_papi_start( pg_rank );
#endif /*PAPI_MONITOR   */

    MPIU_CHKPMEM_COMMIT();
 fn_exit:
    return mpi_errno;
 fn_fail:
    /* --BEGIN ERROR HANDLING-- */
    MPIU_CHKPMEM_REAP();
    goto fn_exit;
    /* --END ERROR HANDLING-- */
}



/* get_local_procs() determines which processes are local and should use shared memory

   OUT
     num_local -- number of local processes
     local_procs -- array of global ranks of local processes
     local_rank -- our local rank

   This uses PMI to get all of the processes that have the same
   hostname, and puts them into local_procs sorted by global rank.
*/
#undef FUNCNAME
#define FUNCNAME get_local_procs
#undef FCNAME
#define FCNAME MPIDI_QUOTE(FUNCNAME)
int
get_local_procs (int global_rank, int num_global, int *num_local_p, int **local_procs_p, int *local_rank_p, int *num_nodes_p, int **node_ids_p)
{
    int mpi_errno = MPI_SUCCESS;
    int ret;
    int val;
    int pmi_errno;
    int *procs;
    int i, j;
    char key[MPID_NEM_MAX_KEY_VAL_LEN];
    char *kvs_name;
    char **node_names;
    char *node_name_buf;
    int *node_ids;
    int num_nodes;
    int num_local;
    int no_local = 0;
    int odd_even_cliques = 0;
    MPIU_CHKPMEM_DECL(2);
    MPIU_CHKLMEM_DECL(2);

    /* Used for debugging only.  This disables communication over
       shared memory */
#ifdef ENABLED_NO_LOCAL
    no_local = 1;
#else
    ret = MPIU_GetEnvBool("MPICH_NO_LOCAL", &val);
    if (ret == 1 && val)
        no_local = 1;
#endif

    /* Used for debugging on a single machine: Odd procs on a node are
       seen as local to each other, and even procs on a node are seen
       as local to each other. */
#ifdef ENABLED_ODD_EVEN_CLIQUES
    odd_even_cliques = 1;
#else
    ret = MPIU_GetEnvBool("MPICH_ODD_EVEN_CLIQUES", &val);
    if (ret == 1 && val)
        odd_even_cliques = 1;
#endif

    if (no_local)
    {
        mpi_errno = get_local_procs_nolocal(global_rank, num_global, num_local_p, local_procs_p, local_rank_p, num_nodes_p, node_ids_p);
        goto fn_exit;
    }

    mpi_errno = MPIDI_PG_GetConnKVSname (&kvs_name);
    if (mpi_errno) MPIU_ERR_POP (mpi_errno);

    /* Put my hostname id */
    if (num_global > 1)
    {
        memset (key, 0, MPID_NEM_MAX_KEY_VAL_LEN);
        MPIU_Snprintf (key, MPID_NEM_MAX_KEY_VAL_LEN, "hostname[%d]", global_rank);

        pmi_errno = PMI_KVS_Put (kvs_name, key, MPID_nem_hostname);
        MPIU_ERR_CHKANDJUMP1 (pmi_errno != PMI_SUCCESS, mpi_errno, MPI_ERR_OTHER, "**pmi_kvs_put", "**pmi_kvs_put %d", pmi_errno);

        pmi_errno = PMI_KVS_Commit (kvs_name);
        MPIU_ERR_CHKANDJUMP1 (pmi_errno != PMI_SUCCESS, mpi_errno, MPI_ERR_OTHER, "**pmi_kvs_commit", "**pmi_kvs_commit %d", pmi_errno);

        pmi_errno = PMI_Barrier();
        MPIU_ERR_CHKANDJUMP1 (pmi_errno != PMI_SUCCESS, mpi_errno, MPI_ERR_OTHER, "**pmi_barrier", "**pmi_barrier %d", pmi_errno);
    }

    /* allocate structures */
    MPIU_CHKPMEM_MALLOC (procs, int *, num_global * sizeof (int), mpi_errno, "local process index array");
    MPIU_CHKPMEM_MALLOC (node_ids, int *, num_global * sizeof (int), mpi_errno, "node_ids");
    MPIU_CHKLMEM_MALLOC (node_names, char **, num_global * sizeof (char*), mpi_errno, "node_names");
    MPIU_CHKLMEM_MALLOC (node_name_buf, char *, num_global * MPID_NEM_MAX_KEY_VAL_LEN * sizeof(char), mpi_errno, "node_name_buf");

    /* Gather hostnames */
    for (i = 0; i < num_global; ++i)
    {
        node_names[i] = &node_name_buf[i * MPID_NEM_MAX_KEY_VAL_LEN];
        node_names[i][0] = '\0';
    }

    num_nodes = 0;
    num_local = 0;

    for (i = 0; i < num_global; ++i)
    {
        if (i == global_rank)
        {
            /* This is us, no need to perform a get */
            MPIU_Snprintf(node_names[num_nodes], MPID_NEM_MAX_KEY_VAL_LEN, "%s", MPID_nem_hostname);
        }
        else
        {
            memset (key, 0, MPID_NEM_MAX_KEY_VAL_LEN);
            MPIU_Snprintf (key, MPID_NEM_MAX_KEY_VAL_LEN, "hostname[%d]", i);

            pmi_errno = PMI_KVS_Get (kvs_name, key, node_names[num_nodes], MPID_NEM_MAX_KEY_VAL_LEN);
            MPIU_ERR_CHKANDJUMP1 (pmi_errno != PMI_SUCCESS, mpi_errno, MPI_ERR_OTHER, "**pmi_kvs_get", "**pmi_kvs_get %d", pmi_errno);
	}

	if (!strncmp (MPID_nem_hostname, node_names[num_nodes], MPID_NEM_MAX_KEY_VAL_LEN)
            && (!odd_even_cliques || (global_rank & 0x1) == (i & 0x1)))
	{
	    if (i == global_rank)
		*local_rank_p = num_local;
	    procs[num_local] = i;
	    ++num_local;
	}

        /* find the node_id for this process, or create a new one */
        /* FIXME: need a better algorithm -- this one does O(N^2) strncmp()s! */
        for (j = 0; j < num_nodes; ++j)
            if (!strncmp (node_names[j], node_names[num_nodes], MPID_NEM_MAX_KEY_VAL_LEN))
                break;
        if (j == num_nodes)
            ++num_nodes;
        else
            node_names[num_nodes][0] = '\0';
        node_ids[i] = j;
    }

    if (odd_even_cliques)
    {
        /* create new processes for all odd numbered processes */
        /* this may leave nodes ids with no processes assigned to them, but I think this is OK*/
        for (i = 0; i < num_global; ++i)
            if (i & 0x1)
                node_ids[i] += num_nodes;
        num_nodes *= 2;
    }

    MPIU_Assert (num_local > 0); /* there's always at least one process */

    /* reduce size of local process array */
    *local_procs_p = MPIU_Realloc (procs, num_local * sizeof (int));
    /* --BEGIN ERROR HANDLING-- */
    if (*local_procs_p == NULL)
    {
        MPIU_CHKMEM_SETERR (mpi_errno, num_local * sizeof (int), "local process index array");
        goto fn_fail;
    }
    /* --END ERROR HANDLING-- */

    *num_local_p = num_local;
    *node_ids_p = node_ids;
    *num_nodes_p = num_nodes;

    MPIU_CHKPMEM_COMMIT();
 fn_exit:
     MPIU_CHKLMEM_FREEALL();
   return mpi_errno;
 fn_fail:
    /* --BEGIN ERROR HANDLING-- */
    MPIU_CHKPMEM_REAP();
    goto fn_exit;
    /* --END ERROR HANDLING-- */
}

#undef FUNCNAME
#define FUNCNAME get_local_procs_nolocal
#undef FCNAME
#define FCNAME MPIDI_QUOTE(FUNCNAME)
static int
get_local_procs_nolocal(int global_rank, int num_global, int *num_local_p, int **local_procs_p, int *local_rank_p, int *num_nodes_p, int **node_ids_p)
{
    /* used for debugging only */
    /* return an array as if there are no other processes on this processor */
    int mpi_errno = MPI_SUCCESS;
    int i;
    MPIU_CHKPMEM_DECL(2);

    *num_local_p = 1;
    *local_rank_p = 0;
    *num_nodes_p = num_global;

    MPIU_CHKPMEM_MALLOC (*local_procs_p, int *, *num_local_p * sizeof (int), mpi_errno, "local proc array");
    **local_procs_p = global_rank;

    MPIU_CHKPMEM_MALLOC (*node_ids_p, int *, num_global * sizeof (int), mpi_errno, "node_ids array");
    for (i = 0; i < num_global; ++i)
        (*node_ids_p)[i] = i;

    MPIU_CHKPMEM_COMMIT();
 fn_exit:
    return mpi_errno;
 fn_fail:
    /* --BEGIN ERROR HANDLING-- */
    MPIU_CHKPMEM_REAP();
    goto fn_exit;
    /* --END ERROR HANDLING-- */

}


/* MPID_nem_vc_init initialize nemesis' part of the vc */
#undef FUNCNAME
#define FUNCNAME MPID_nem_vc_init
#undef FCNAME
#define FCNAME MPIDI_QUOTE(FUNCNAME)
int
MPID_nem_vc_init (MPIDI_VC_t *vc)
{
    int mpi_errno = MPI_SUCCESS;
    MPIDI_CH3I_VC *vc_ch = (MPIDI_CH3I_VC *)vc->channel_private;
    MPIU_CHKPMEM_DECL(1);
    MPIDI_STATE_DECL(MPID_STATE_MPID_NEM_VC_INIT);

    MPIDI_FUNC_ENTER(MPID_STATE_MPID_NEM_VC_INIT);
    vc_ch->send_seqno = 0;

    vc_ch->pending_pkt_len = 0;
    MPIU_CHKPMEM_MALLOC (vc_ch->pending_pkt, MPIDI_CH3_PktGeneric_t *, sizeof (MPIDI_CH3_PktGeneric_t), mpi_errno, "pending_pkt");

    /* We do different things for vcs in the COMM_WORLD pg vs other pgs
       COMM_WORLD vcs may use shared memory, and already have queues allocated
    */
    if (vc->lpid < MPID_nem_mem_region.num_procs)
    {
	/* This vc is in COMM_WORLD */
	vc_ch->is_local = MPID_NEM_IS_LOCAL (vc->lpid);
	vc_ch->free_queue = MPID_nem_mem_region.FreeQ[vc->lpid]; /* networks and local procs have free queues */
        vc_ch->node_id = MPID_nem_mem_region.node_ids[vc->lpid];
    }
    else
    {
	/* this vc is the result of a connect */
	vc_ch->is_local = 0;
	vc_ch->free_queue = net_free_queue;
        vc_ch->node_id = -1; /* we're not using shared memory, so assume we're on our own node */
    }

    /* override rendezvous functions */
    vc->rndvSend_fn = MPID_nem_lmt_RndvSend;
    vc->rndvRecv_fn = MPID_nem_lmt_RndvRecv;

    if (vc_ch->is_local)
    {
	vc_ch->fbox_out = &MPID_nem_mem_region.mailboxes.out[MPID_nem_mem_region.local_ranks[vc->lpid]]->mpich2;
	vc_ch->fbox_in = &MPID_nem_mem_region.mailboxes.in[MPID_nem_mem_region.local_ranks[vc->lpid]]->mpich2;
	vc_ch->recv_queue = MPID_nem_mem_region.RecvQ[vc->lpid];

        /* override nocontig send function */
        vc->sendNoncontig_fn = MPIDI_CH3I_SendNoncontig;

        /* local processes use the default method */
        vc_ch->iStartContigMsg = NULL;
        vc_ch->iSendContig     = NULL;

        vc_ch->lmt_initiate_lmt  = MPID_nem_lmt_shm_initiate_lmt;
        vc_ch->lmt_start_recv    = MPID_nem_lmt_shm_start_recv;
        vc_ch->lmt_start_send    = MPID_nem_lmt_shm_start_send;
        vc_ch->lmt_handle_cookie = MPID_nem_lmt_shm_handle_cookie;
        vc_ch->lmt_done_send     = MPID_nem_lmt_shm_done_send;
        vc_ch->lmt_done_recv     = MPID_nem_lmt_shm_done_recv;

        vc_ch->lmt_copy_buf        = NULL;
        vc_ch->lmt_copy_buf_handle = NULL;
        vc_ch->lmt_queue.head      = NULL;
        vc_ch->lmt_queue.tail      = NULL;
        vc_ch->lmt_active_lmt      = NULL;
        vc_ch->lmt_enqueued        = FALSE;

        vc->eager_max_msg_sz = MPID_NEM_MPICH2_DATA_LEN - sizeof(MPIDI_CH3_Pkt_t);

        MPIU_DBG_MSG(VC, VERBOSE, "vc using shared memory");
    }
    else
    {
	vc_ch->fbox_out   = NULL;
	vc_ch->fbox_in    = NULL;
	vc_ch->recv_queue = NULL;

        vc_ch->lmt_initiate_lmt  = NULL;
        vc_ch->lmt_start_recv    = NULL;
        vc_ch->lmt_start_send    = NULL;
        vc_ch->lmt_handle_cookie = NULL;
        vc_ch->lmt_done_send     = NULL;
        vc_ch->lmt_done_recv     = NULL;

        /* FIXME: DARIUS set these to default for now */
        vc_ch->iStartContigMsg = NULL;
        vc_ch->iSendContig     = NULL;

        MPIU_DBG_MSG_FMT(VC, VERBOSE, (MPIU_DBG_FDEST, "vc using %s netmod for rank %d pg %s",
                                       MPID_nem_netmod_strings[MPID_nem_netmod_id], vc->pg_rank,
                                       (vc->pg == MPIDI_Process.my_pg ? "my_pg" : (char *)vc->pg->id)));
        
        mpi_errno = MPID_nem_netmod_func->vc_init(vc);
	if (mpi_errno) MPIU_ERR_POP(mpi_errno);

/* FIXME: DARIUS -- enable this assert once these functions are implemented */
/*         /\* iStartContigMsg iSendContig and sendNoncontig_fn must */
/*            be set for nonlocal processes.  Default functions only */
/*            support shared-memory communication. *\/ */
/*         MPIU_Assert(vc_ch->iStartContigMsg && vc_ch->iSendContig && vc->sendNoncontig_fn); */

    }

    /* FIXME: ch3 assumes there is a field called sendq_head in the ch
       portion of the vc.  This is unused in nemesis and should be set
       to NULL */
    vc_ch->sendq_head = NULL;

    MPIU_CHKPMEM_COMMIT();
fn_exit:
    MPIDI_FUNC_EXIT(MPID_STATE_MPID_NEM_VC_INIT);
    return mpi_errno;
 fn_fail:
    MPIU_CHKPMEM_REAP();
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPID_nem_vc_destroy
#undef FCNAME
#define FCNAME MPIDI_QUOTE(FUNCNAME)
int
MPID_nem_vc_destroy(MPIDI_VC_t *vc)
{
    int mpi_errno = MPI_SUCCESS;
    MPIDI_CH3I_VC *vc_ch = (MPIDI_CH3I_VC *)vc->channel_private;
    MPIDI_STATE_DECL(MPID_STATE_MPID_NEM_VC_DESTROY);

    MPIDI_FUNC_ENTER(MPID_STATE_MPID_NEM_VC_DESTROY);

    MPIU_Free(vc_ch->pending_pkt);

    mpi_errno = MPID_nem_netmod_func->vc_destroy(vc);
    if (mpi_errno) MPIU_ERR_POP(mpi_errno);

    fn_exit:
    MPIDI_FUNC_EXIT(MPID_STATE_MPID_NEM_VC_DESTROY);
    return mpi_errno;
 fn_fail:
    goto fn_exit;
}

int
MPID_nem_get_business_card (int my_rank, char *value, int length)
{
    return MPID_nem_netmod_func->get_business_card (my_rank, &value, &length);
}

int MPID_nem_connect_to_root (const char *business_card, MPIDI_VC_t *new_vc)
{
    return MPID_nem_netmod_func->connect_to_root (business_card, new_vc);
}
