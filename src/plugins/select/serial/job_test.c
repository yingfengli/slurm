/*****************************************************************************\
 *  job_test.c
 *****************************************************************************
 *  Copyright (C) 2005-2008 Hewlett-Packard Development Company, L.P.
 *  Written by Susanne M. Balle <susanne.balle@hp.com>, who borrowed heavily
 *  from select/linear
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.schedmd.com/slurmdocs/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#  if HAVE_STDINT_H
#    include <stdint.h>
#  endif
#  if HAVE_INTTYPES_H
#    include <inttypes.h>
#  endif
#endif
#include <time.h>

#include "dist_tasks.h"
#include "job_test.h"
#include "select_serial.h"

static int _eval_nodes(struct job_record *job_ptr, bitstr_t *node_map,
			uint32_t cr_node_cnt, uint16_t *cpu_cnt);

/* _allocate_sockets - Given the job requirements, determine which sockets
 *                     from the given node can be allocated (if any) to this
 *                     job. Returns the number of cpus that can be used by
 *                     this node AND a core-level bitmap of the selected
 *                     sockets.
 *
 * IN job_ptr      - pointer to job requirements
 * IN/OUT core_map - core_bitmap of available cores
 * IN node_i       - index of node to be evaluated
 */
uint16_t _allocate_sockets(struct job_record *job_ptr, bitstr_t *core_map,
			   const uint32_t node_i)
{
	uint16_t cpu_count = 0, cpu_cnt = 0;
	uint16_t si, cps, avail_cpus = 0, num_tasks = 0;
	uint32_t core_begin    = cr_get_coremap_offset(node_i);
	uint32_t core_end      = cr_get_coremap_offset(node_i+1);
	uint32_t c;
	uint16_t cpus_per_task = job_ptr->details->cpus_per_task;
	uint16_t *used_cores, *free_cores, free_core_count = 0;
	uint16_t i, j, sockets    = select_node_record[node_i].sockets;
	uint16_t cores_per_socket = select_node_record[node_i].cores;
	uint16_t threads_per_core = select_node_record[node_i].vpus;
	uint16_t min_cores = 1, min_sockets = 1, ntasks_per_socket = 0;
	uint16_t ntasks_per_core = 0xffff;

	if (job_ptr->details && job_ptr->details->mc_ptr) {
		multi_core_data_t *mc_ptr = job_ptr->details->mc_ptr;
		if (mc_ptr->cores_per_socket != (uint16_t) NO_VAL) {
			min_cores = mc_ptr->cores_per_socket;
		}
		if (mc_ptr->sockets_per_node != (uint16_t) NO_VAL){
			min_sockets = mc_ptr->sockets_per_node;
		}
		if (mc_ptr->ntasks_per_core) {
			ntasks_per_core = mc_ptr->ntasks_per_core;
		}
		if ((mc_ptr->threads_per_core != (uint16_t) NO_VAL) &&
		    (mc_ptr->threads_per_core <  ntasks_per_core)) {
			ntasks_per_core = mc_ptr->threads_per_core;
		}
		ntasks_per_socket = mc_ptr->ntasks_per_socket;
	}

	/* These are the job parameters that we must respect:
	 *
	 *   job_ptr->details->mc_ptr->cores_per_socket (cr_core|cr_socket)
	 *	- min # of cores per socket to allocate to this job
	 *   job_ptr->details->mc_ptr->sockets_per_node (cr_core|cr_socket)
	 *	- min # of sockets per node to allocate to this job
	 *   job_ptr->details->mc_ptr->ntasks_per_core (cr_core|cr_socket)
	 *	- number of tasks to launch per core
	 *   job_ptr->details->mc_ptr->ntasks_per_socket (cr_core|cr_socket)
	 *	- number of tasks to launch per socket
	 *
	 *   job_ptr->details->ntasks_per_node (all cr_types)
	 *	- total number of tasks to launch on this node
	 *   job_ptr->details->cpus_per_task (all cr_types)
	 *	- number of cpus to allocate per task
	 *
	 * These are the hardware constraints:
	 *   cpus = sockets * cores_per_socket * threads_per_core
	 *
	 * These are the cores/sockets that are available: core_map
	 *
	 * NOTE: currently we only allocate at the socket level, the core
	 *       level, or the cpu level. When hyperthreading is enabled
	 *       in the BIOS, then there can be more than one thread/cpu
	 *       per physical core.
	 *
	 * PROCEDURE:
	 *
	 * Step 1: Determine the current usage data: used_cores[],
	 *         used_core_count, free_cores[], free_core_count
	 *
	 * Step 2: For core-level and socket-level: apply sockets_per_node
	 *         and cores_per_socket to the "free" cores.
	 *
	 * Step 3: Compute task-related data: ntasks_per_core,
	 *         ntasks_per_socket, ntasks_per_node and cpus_per_task
	 *         and determine the number of tasks to run on this node
	 *
	 * Step 4: Mark the allocated resources in the job_cores bitmap
	 *         and return "num_tasks" from Step 3.
	 *
	 *
	 * For socket and core counts, start by assuming that all available
	 * resources will be given to the job. Check min_* to ensure that
	 * there's enough resources. Reduce the resource count to match max_*
	 * (if necessary). Also reduce resource count (if necessary) to
	 * match ntasks_per_resource.
	 *
	 * NOTE: Memory is not used as a constraint here - should it?
	 *       If not then it needs to be done somewhere else!
	 */


	/* Step 1: create and compute core-count-per-socket
	 * arrays and total core counts */
	free_cores = xmalloc(sockets * sizeof(uint16_t));
	used_cores = xmalloc(sockets * sizeof(uint16_t));

	for (c = core_begin; c < core_end; c++) {
		i = (uint16_t) (c - core_begin) / cores_per_socket;
		if (bit_test(core_map, c)) {
			free_cores[i]++;
			free_core_count++;
		} else {
			used_cores[i]++;
		}
	}
	/* if a socket is already in use, it cannot be used
	 * by this job */
	for (i = 0; i < sockets; i++) {
		if (used_cores[i]) {
			free_core_count -= free_cores[i];
			used_cores[i] += free_cores[i];
			free_cores[i] = 0;
		}
	}
	xfree(used_cores);
	used_cores = NULL;

	/* Step 2: check min_cores per socket and min_sockets per node */
	j = 0;
	for (i = 0; i < sockets; i++) {
		if (free_cores[i] < min_cores) {
			/* cannot use this socket */
			free_core_count -= free_cores[i];
			free_cores[i] = 0;
			continue;
		}
		/* count this socket as usable */
		j++;
	}
	if (j < min_sockets) {
		/* cannot use this node */
		num_tasks = 0;
		goto fini;
	}

	if (free_core_count < 1) {
		/* no available resources on this node */
		num_tasks = 0;
		goto fini;
	}

	/* Step 3: Compute task-related data:
	 *         ntasks_per_socket, ntasks_per_node and cpus_per_task
	 *         to determine the number of tasks to run on this node
	 *
	 * Note: cpus_per_task and ntasks_per_core need to play nice
	 *       2 tasks_per_core vs. 2 cpus_per_task
	 */
	avail_cpus = 0;
	num_tasks = 0;
	threads_per_core = MIN(threads_per_core, ntasks_per_core);

	for (i = 0; i < sockets; i++) {
		uint16_t tmp = free_cores[i] * threads_per_core;
		avail_cpus += tmp;
		if (ntasks_per_socket)
			num_tasks += MIN(tmp, ntasks_per_socket);
		else
			num_tasks += tmp;
	}

	/* If job requested exclusive rights to the node don't do the
	 * min here since it will make it so we don't allocate the
	 * entire node. */
	if (job_ptr->details->ntasks_per_node && job_ptr->details->shared)
		num_tasks = MIN(num_tasks, job_ptr->details->ntasks_per_node);

	if (cpus_per_task < 2) {
		avail_cpus = num_tasks;
		cps = num_tasks;
	} else {
		j = avail_cpus / cpus_per_task;
		num_tasks = MIN(num_tasks, j);
		if (job_ptr->details->ntasks_per_node)
			avail_cpus = num_tasks * cpus_per_task;
	}
	if ((job_ptr->details->ntasks_per_node &&
	     (num_tasks < job_ptr->details->ntasks_per_node)) ||
	    (job_ptr->details->pn_min_cpus &&
	     (avail_cpus < job_ptr->details->pn_min_cpus))) {
		/* insufficient resources on this node */
		num_tasks = 0;
		goto fini;
	}

	/* Step 4 - make sure that ntasks_per_socket is enforced when
	 *          allocating cores
	 */
	cps = num_tasks;
	if (ntasks_per_socket > 1) {
		cps = ntasks_per_socket;
		if (cpus_per_task > 1)
			cps = ntasks_per_socket * cpus_per_task;
	}
	si = 9999;
	for (c = core_begin; c < core_end && avail_cpus > 0; c++) {
		if (bit_test(core_map, c) == 0)
			continue;
		i = (uint16_t) (c - core_begin) / cores_per_socket;
		if (free_cores[i] > 0) {
			/* this socket has free cores, but make sure
			 * we don't use more than are needed for
			 * ntasks_per_socket */
			if (si != i) {
				si = i;
				cpu_cnt = threads_per_core;
			} else {
				if (cpu_cnt >= cps) {
					/* do not allocate this core */
					bit_clear(core_map, c);
					continue;
				}
				cpu_cnt += threads_per_core;
			}
			free_cores[i]--;
			/* we have to ensure that cpu_count 
			 * is not bigger than avail_cpus due to 
			 * hyperthreading or this would break
			 * the selection logic providing more
			 * cpus than allowed after task-related data
			 * processing of stage 3
			 */
			if (avail_cpus >= threads_per_core) {
				avail_cpus -= threads_per_core;
				cpu_count += threads_per_core;
			}
			else {
				cpu_count += avail_cpus;
				avail_cpus = 0;
			}

		} else
			bit_clear(core_map, c);
	}
	/* clear leftovers */
	if (c < core_end)
		bit_nclear(core_map, c, core_end-1);

fini:
	/* if num_tasks == 0 then clear all bits on this node */
	if (!num_tasks) {
		bit_nclear(core_map, core_begin, core_end-1);
		cpu_count = 0;
	}
	xfree(free_cores);
	return cpu_count;
}


/* _allocate_cores - Given the job requirements, determine which cores
 *                   from the given node can be allocated (if any) to this
 *                   job. Returns the number of cpus that can be used by
 *                   this node AND a bitmap of the selected cores.
 *
 * IN job_ptr      - pointer to job requirements
 * IN/OUT core_map - bitmap of cores available for use/selected for use
 * IN node_i       - index of node to be evaluated
 * IN cpu_type     - if true, allocate CPUs rather than cores
 */
uint16_t _allocate_cores(struct job_record *job_ptr, bitstr_t *core_map,
			 const uint32_t node_i, bool cpu_type)
{
	uint16_t cpu_count = 0, avail_cpus = 0, num_tasks = 0;
	uint32_t core_begin    = cr_get_coremap_offset(node_i);
	uint32_t core_end      = cr_get_coremap_offset(node_i+1);
	uint32_t c;
	uint16_t cpus_per_task = job_ptr->details->cpus_per_task;
	uint16_t *free_cores, free_core_count = 0;
	uint16_t i, j, sockets    = select_node_record[node_i].sockets;
	uint16_t cores_per_socket = select_node_record[node_i].cores;
	uint16_t threads_per_core = select_node_record[node_i].vpus;
	uint16_t min_cores = 1, min_sockets = 1;
	uint16_t ntasks_per_core = 0xffff;

	if (job_ptr->details && job_ptr->details->mc_ptr) {
		multi_core_data_t *mc_ptr = job_ptr->details->mc_ptr;
		if (mc_ptr->cores_per_socket != (uint16_t) NO_VAL) {
			min_cores   = mc_ptr->cores_per_socket;
		}
		if (mc_ptr->sockets_per_node != (uint16_t) NO_VAL){
			min_sockets = mc_ptr->sockets_per_node;
		}
		if (mc_ptr->ntasks_per_core) {
			ntasks_per_core = mc_ptr->ntasks_per_core;
		}
		if ((mc_ptr->threads_per_core != (uint16_t) NO_VAL) &&
		    (mc_ptr->threads_per_core <  ntasks_per_core)) {
			ntasks_per_core = mc_ptr->threads_per_core;
		}
	}

	/* These are the job parameters that we must respect:
	 *
	 *   job_ptr->details->mc_ptr->cores_per_socket (cr_core|cr_socket)
	 *	- number of cores per socket to allocate to this jobb
	 *   job_ptr->details->mc_ptr->sockets_per_node (cr_core|cr_socket)
	 *	- number of sockets per node to allocate to this job
	 *   job_ptr->details->mc_ptr->ntasks_per_core (cr_core|cr_socket)
	 *	- number of tasks to launch per core
	 *   job_ptr->details->mc_ptr->ntasks_per_socket (cr_core|cr_socket)
	 *	- number of tasks to launch per socket
	 *
	 *   job_ptr->details->ntasks_per_node (all cr_types)
	 *	- total number of tasks to launch on this node
	 *   job_ptr->details->cpus_per_task (all cr_types)
	 *	- number of cpus to allocate per task
	 *
	 * These are the hardware constraints:
	 *   cpus = sockets * cores_per_socket * threads_per_core
	 *
	 * These are the cores that are available for use: core_map
	 *
	 * NOTE: currently we only allocate at the socket level, the core
	 *       level, or the cpu level. When hyperthreading is enabled
	 *       in the BIOS, then there can be more than one thread/cpu
	 *       per physical core.
	 *
	 * PROCEDURE:
	 *
	 * Step 1: Determine the current usage data: free_cores[] and
	 *         free_core_count
	 *
	 * Step 2: check min_cores per socket and min_sockets per node
	 *
	 * Step 3: Compute task-related data: use ntasks_per_core,
	 *         ntasks_per_node and cpus_per_task to determine
	 *         the number of tasks that can run on this node
	 *
	 * Step 4: Mark the allocated resources in the job_cores bitmap
	 *         and return "num_tasks" from Step 3.
	 *
	 *
	 * Start by assuming that all "free" cores will be given to the
	 * job. Check min_* to ensure that there's enough resources.
	 * Reduce the core count to match max_* (if necessary). Also,
	 * reduce the core count (if necessary) to match ntasks_per_core.
	 * Note that we're not processing ntasks_per_socket, because the
	 * srun manpage says that ntasks_per_socket is only valid for
	 * CR_SOCKET.
	 */

	/* Step 1: create and compute core-count-per-socket
	 * arrays and total core counts */
	free_cores = xmalloc(sockets * sizeof(uint16_t));

	for (c = core_begin; c < core_end; c++) {
		i = (uint16_t) (c - core_begin) / cores_per_socket;
		if (bit_test(core_map, c)) {
			free_cores[i]++;
			free_core_count++;
		}
	}

	/* Step 2: check min_cores per socket and min_sockets per node */
	j = 0;
	for (i = 0; i < sockets; i++) {
		if (free_cores[i] < min_cores) {
			/* cannot use this socket */
			free_core_count -= free_cores[i];
			free_cores[i] = 0;
			continue;
		}
		/* count this socket as usable */
		j++;
	}
	if (j < min_sockets) {
		/* cannot use this node */
		num_tasks = 0;
		goto fini;
	}

	if (free_core_count < 1) {
		/* no available resources on this node */
		num_tasks = 0;
		goto fini;
	}

	/* Step 3: Compute task-related data: use ntasks_per_core,
	 *         ntasks_per_node and cpus_per_task to determine
	 *         the number of tasks to run on this node
	 *
	 * Note: cpus_per_task and ntasks_per_core need to play nice
	 *       2 tasks_per_core vs. 2 cpus_per_task
	 */
	threads_per_core = MIN(threads_per_core, ntasks_per_core);
	num_tasks = avail_cpus = threads_per_core;

	/* convert from PER_CORE to TOTAL_FOR_NODE */
	avail_cpus *= free_core_count;
	num_tasks  *= free_core_count;

	/* If job requested exclusive rights to the node don't do the min here
	 * since it will make it so we don't allocate the entire node */
	if (job_ptr->details->ntasks_per_node && job_ptr->details->shared)
		num_tasks = MIN(num_tasks, job_ptr->details->ntasks_per_node);

	if (cpus_per_task < 2) {
		avail_cpus = num_tasks;
	} else {
		j = avail_cpus / cpus_per_task;
		num_tasks = MIN(num_tasks, j);
		if (job_ptr->details->ntasks_per_node)
			avail_cpus = num_tasks * cpus_per_task;
	}

	if ((job_ptr->details->ntasks_per_node &&
	     (num_tasks < job_ptr->details->ntasks_per_node) &&
	     (job_ptr->details->overcommit == 0)) ||
	    (job_ptr->details->pn_min_cpus &&
	     (avail_cpus < job_ptr->details->pn_min_cpus))) {
		/* insufficient resources on this node */
		num_tasks = 0;
		goto fini;
	}

	/* Step 4 */
	for (c = core_begin; c < core_end && avail_cpus > 0; c++) {
		if (bit_test(core_map, c) == 0)
			continue;
		i = (uint16_t) (c - core_begin) / cores_per_socket;
		if (free_cores[i] == 0)
			bit_clear(core_map, c);
		else {
			free_cores[i]--;
			/* we have to ensure that cpu_count 
			 * is not bigger than avail_cpus due to 
			 * hyperthreading or this would break
			 * the selection logic providing more
			 * cpus than allowed after task-related data
			 * processing of stage 3
			 */
			if (avail_cpus >= threads_per_core) {
				avail_cpus -= threads_per_core;
				cpu_count += threads_per_core;
			}
			else {
				cpu_count += avail_cpus;
				avail_cpus = 0;
			}
		}
	}

	/* clear leftovers */
	if (c < core_end)
		bit_nclear(core_map, c, core_end-1);

fini:
	if (!num_tasks) {
		bit_nclear(core_map, core_begin, core_end-1);
		cpu_count = 0;
	}
	xfree(free_cores);
	return cpu_count;
}


/*
 * _can_job_run_on_node - Given the job requirements, determine which
 *                        resources from the given node (if any) can be
 *                        allocated to this job. Returns the number of
 *                        cpus that can be used by this node and a bitmap
 *                        of available resources for allocation.
 *       NOTE: This process does NOT support overcommitting resources
 *
 * IN job_ptr       - pointer to job requirements
 * IN/OUT core_map  - core_bitmap of available cores
 * IN n             - index of node to be evaluated
 * IN cr_type       - Consumable Resource setting
 * IN test_only     - ignore allocated memory check
 *
 * NOTE: The returned cpu_count may be less than the number of set bits in
 *       core_map for the given node. The cr_dist functions will determine
 *       which bits to deselect from the core_map to match the cpu_count.
 */
uint16_t _can_job_run_on_node(struct job_record *job_ptr, bitstr_t *core_map,
			      const uint32_t node_i,
			      struct node_use_record *node_usage,
			      uint16_t cr_type,
			      bool test_only)
{
	uint16_t cpus;
	uint32_t avail_mem, req_mem, gres_cpus;
	int core_start_bit, core_end_bit;
	struct node_record *node_ptr = node_record_table_ptr + node_i;
	List gres_list;

	if (!test_only && IS_NODE_COMPLETING(node_ptr)) {
		/* Do not allocate more jobs to nodes with completing jobs */
		cpus = 0;
		return cpus;
	}

	if (cr_type & CR_CORE)
		cpus = _allocate_cores(job_ptr, core_map, node_i, false);
	else if (cr_type & CR_SOCKET)
		cpus = _allocate_sockets(job_ptr, core_map, node_i);
	else
		cpus = _allocate_cores(job_ptr, core_map, node_i, true);

	core_start_bit = cr_get_coremap_offset(node_i);
	core_end_bit   = cr_get_coremap_offset(node_i+1) - 1;
	node_ptr = select_node_record[node_i].node_ptr;

	if (cr_type & CR_MEMORY) {
		/* Memory Check: check pn_min_memory to see if:
		 *          - this node has enough memory (MEM_PER_CPU == 0)
		 *          - there are enough free_cores (MEM_PER_CPU = 1)
		 */
		req_mem   = job_ptr->details->pn_min_memory & ~MEM_PER_CPU;
		avail_mem = select_node_record[node_i].real_memory;
		if (!test_only)
			avail_mem -= node_usage[node_i].alloc_memory;
		if (job_ptr->details->pn_min_memory & MEM_PER_CPU) {
			/* memory is per-cpu */
			while ((cpus > 0) && ((req_mem * cpus) > avail_mem))
				cpus--;
			if ((cpus < job_ptr->details->ntasks_per_node) ||
			    ((job_ptr->details->cpus_per_task > 1) &&
			     (cpus < job_ptr->details->cpus_per_task)))
				cpus = 0;
			/* FIXME: Need to recheck min_cores, etc. here */
		} else {
			/* memory is per node */
			if (req_mem > avail_mem)
				cpus = 0;
		}
	}

	if (node_usage[node_i].gres_list)
		gres_list = node_usage[node_i].gres_list;
	else
		gres_list = node_ptr->gres_list;
	gres_cpus = gres_plugin_job_test(job_ptr->gres_list,
					 gres_list, test_only,
					 core_map, core_start_bit,
					 core_end_bit, job_ptr->job_id,
					 node_ptr->name);
	if ((gres_cpus < job_ptr->details->ntasks_per_node) ||
	    ((job_ptr->details->cpus_per_task > 1) &&
	     (gres_cpus < job_ptr->details->cpus_per_task)))
		gres_cpus = 0;
	if (gres_cpus < cpus)
		cpus = gres_cpus;

	if (cpus == 0)
		bit_nclear(core_map, core_start_bit, core_end_bit);

	if (select_debug_flags & DEBUG_FLAG_CPU_BIND) {
		info("cons_res: _can_job_run_on_node: %u cpus on %s(%d), "
		     "mem %u/%u",
		     cpus, select_node_record[node_i].node_ptr->name,
		     node_usage[node_i].node_state,
		     node_usage[node_i].alloc_memory,
		     select_node_record[node_i].real_memory);
	}

	return cpus;
}


/* Test to see if a node already has running jobs for _other_ partitions.
 * If (sharing_only) then only check sharing partitions. This is because
 * the job was submitted to a single-row partition which does not share
 * allocated CPUs with multi-row partitions.
 */
static int _is_node_busy(struct part_res_record *p_ptr, uint32_t node_i,
			 int sharing_only, struct part_record *my_part_ptr)
{
	uint32_t r, cpu_begin = cr_get_coremap_offset(node_i);
	uint32_t i, cpu_end   = cr_get_coremap_offset(node_i+1);

	for (; p_ptr; p_ptr = p_ptr->next) {
		if (sharing_only && 
		    ((p_ptr->num_rows < 2) || 
		     (p_ptr->part_ptr == my_part_ptr)))
			continue;
		if (!p_ptr->row)
			continue;
		for (r = 0; r < p_ptr->num_rows; r++) {
			if (!p_ptr->row[r].row_bitmap)
				continue;
			for (i = cpu_begin; i < cpu_end; i++) {
				if (bit_test(p_ptr->row[r].row_bitmap, i))
					return 1;
			}
		}
	}
	return 0;
}


/*
 * Determine which of these nodes are usable by this job
 *
 * Remove nodes from the bitmap that don't have enough memory or gres to
 * support the job. 
 *
 * Return SLURM_ERROR if a required node can't be used.
 *
 * if node_state = NODE_CR_RESERVED, clear bitmap (if node is required
 *                                   then should we return NODE_BUSY!?!)
 *
 * if node_state = NODE_CR_ONE_ROW, then this node can only be used by
 *                                  another NODE_CR_ONE_ROW job
 *
 * if node_state = NODE_CR_AVAILABLE AND:
 *  - job_node_req = NODE_CR_RESERVED, then we need idle nodes
 *  - job_node_req = NODE_CR_ONE_ROW, then we need idle or non-sharing nodes
 */
static int _verify_node_state(struct part_res_record *cr_part_ptr,
			      struct job_record *job_ptr, bitstr_t * bitmap,
			      uint16_t cr_type,
			      struct node_use_record *node_usage,
			      enum node_cr_state job_node_req)
{
	struct node_record *node_ptr;
	uint32_t i, free_mem, gres_cpus, min_mem, size;
	List gres_list;

	if (job_ptr->details->pn_min_memory & MEM_PER_CPU) {
		uint16_t min_cpus;
		min_mem = job_ptr->details->pn_min_memory & (~MEM_PER_CPU);
		min_cpus = MAX(job_ptr->details->ntasks_per_node,
			       job_ptr->details->pn_min_cpus);
		min_cpus = MAX(min_cpus, job_ptr->details->cpus_per_task);
		if (min_cpus > 0)
			min_mem *= min_cpus;
	} else {
		min_mem = job_ptr->details->pn_min_memory;
	}
	size = bit_size(bitmap);
	for (i = 0; i < size; i++) {
		if (!bit_test(bitmap, i))
			continue;
		node_ptr = select_node_record[i].node_ptr;

		/* node-level memory check */
		if ((job_ptr->details->pn_min_memory) &&
		    (cr_type & CR_MEMORY)) {
			free_mem  = select_node_record[i].real_memory;
			free_mem -= node_usage[i].alloc_memory;
			if (free_mem < min_mem) {
				debug3("cons_res: _vns: node %s no mem %u < %u",
					select_node_record[i].node_ptr->name,
					free_mem, min_mem);
				goto clear_bit;
			}
		}

		/* node-level gres check */
		if (node_usage[i].gres_list)
			gres_list = node_usage[i].gres_list;
		else
			gres_list = node_ptr->gres_list;
		gres_cpus = gres_plugin_job_test(job_ptr->gres_list, 
						 gres_list, true,
						 NULL, 0, 0, job_ptr->job_id,
						 node_ptr->name);
		if (gres_cpus == 0) {
			debug3("cons_res: _vns: node %s lacks gres",
			       node_ptr->name);
			goto clear_bit;
		}

		/* exclusive node check */
		if (node_usage[i].node_state >= NODE_CR_RESERVED) {
			debug3("cons_res: _vns: node %s in exclusive use",
			       node_ptr->name);
			goto clear_bit;

		/* non-resource-sharing node check */
		} else if (node_usage[i].node_state >= NODE_CR_ONE_ROW) {
			if ((job_node_req == NODE_CR_RESERVED) ||
			    (job_node_req == NODE_CR_AVAILABLE)) {
				debug3("cons_res: _vns: node %s non-sharing",
				       node_ptr->name);
				goto clear_bit;
			}
			/* cannot use this node if it is running jobs
			 * in sharing partitions */
			if (_is_node_busy(cr_part_ptr, i, 1,
					  job_ptr->part_ptr)) {
				debug3("cons_res: _vns: node %s sharing?",
				       node_ptr->name);
				goto clear_bit;
			}

		/* node is NODE_CR_AVAILABLE - check job request */
		} else {
			if (job_node_req == NODE_CR_RESERVED) {
				if (_is_node_busy(cr_part_ptr, i, 0,
						  job_ptr->part_ptr)) {
					debug3("cons_res: _vns: node %s busy",
					       node_ptr->name);
					goto clear_bit;
				}
			} else if (job_node_req == NODE_CR_ONE_ROW) {
				/* cannot use this node if it is running jobs
				 * in sharing partitions */
				if (_is_node_busy(cr_part_ptr, i, 1, 
						  job_ptr->part_ptr)) {
					debug3("cons_res: _vns: node %s vbusy",
					       node_ptr->name);
					goto clear_bit;
				}
			}
		}
		continue;	/* node is usable, test next node */

clear_bit:	/* This node is not usable by this job */
		bit_clear(bitmap, i);
		if (job_ptr->details->req_node_bitmap &&
		    bit_test(job_ptr->details->req_node_bitmap, i))
			return SLURM_ERROR;

	}

	return SLURM_SUCCESS;
}


/* given an "avail" node_bitmap, return a corresponding "avail" core_bitmap */
bitstr_t *_make_core_bitmap(bitstr_t *node_map)
{
	uint32_t n, c, nodes, size;
	uint32_t coff;

	nodes = bit_size(node_map);
	size = cr_get_coremap_offset(nodes);
	bitstr_t *core_map = bit_alloc(size);
	if (!core_map)
		return NULL;

	nodes = bit_size(node_map);
	for (n = 0, c = 0; n < nodes; n++) {
		if (bit_test(node_map, n)) {
			coff = cr_get_coremap_offset(n+1);
			while (c < coff) {
				bit_set(core_map, c++);
			}
		}
	}
	return core_map;
}

/* Compute resource usage for the given job on all available resources
 *
 * IN: job_ptr     - pointer to the job requesting resources
 * IN: node_map    - bitmap of available nodes
 * IN/OUT: core_map    - bitmap of available cores
 * IN: cr_node_cnt - total number of nodes in the cluster
 * IN: cr_type     - resource type
 * OUT: cpu_cnt    - number of cpus that can be used by this job
 * IN: test_only   - ignore allocated memory check
 */
static void _get_res_usage(struct job_record *job_ptr, bitstr_t *node_map,
			   bitstr_t *core_map, uint32_t cr_node_cnt,
			   struct node_use_record *node_usage,
			   uint16_t cr_type, uint16_t **cpu_cnt_ptr, 
			   bool test_only)
{
	uint16_t *cpu_cnt;
	uint32_t n;

	cpu_cnt = xmalloc(cr_node_cnt * sizeof(uint16_t));
	for (n = 0; n < cr_node_cnt; n++) {
		if (!bit_test(node_map, n))
			continue;
		cpu_cnt[n] = _can_job_run_on_node(job_ptr, core_map, n,
						  node_usage, cr_type,
						  test_only);
	}
	*cpu_cnt_ptr = cpu_cnt;
}

/* this is the heart of the selection process */
static int _eval_nodes(struct job_record *job_ptr, bitstr_t *node_map,
		       uint32_t cr_node_cnt, uint16_t *cpu_cnt)
{
	int i;
	struct job_details *details_ptr = job_ptr->details;

	xassert(cpu_cnt);
	xassert(node_map);
	if (cr_node_cnt != node_record_count) {
		error("select/serial: node count inconsistent with slurmctld");
		return SLURM_ERROR;
	}
	if (details_ptr->req_node_bitmap)
		bit_or(node_map, details_ptr->req_node_bitmap);
	i = bit_ffs(node_map);
	if (i < 0)
		return SLURM_ERROR;
	for ( ; i < node_record_count; i++) {
		if (cpu_cnt[i])
			break;
	}
	if (i >= node_record_count)
		return SLURM_ERROR;
	bit_nclear(node_map, 0, (node_record_count - 1));
	bit_set(node_map, i);
	return SLURM_SUCCESS;
}

/* this is an intermediary step between _select_nodes and _eval_nodes
 * to tackle the knapsack problem. This code incrementally removes nodes
 * with low cpu counts for the job and re-evaluates each result */
static int _choose_nodes(struct job_record *job_ptr, bitstr_t *node_map,
			 uint32_t cr_node_cnt, uint16_t *cpu_cnt)
{
	int i, count, ec, most_cpus = 0;
	bitstr_t *origmap, *reqmap = NULL;

	if (job_ptr->details->req_node_bitmap)
		reqmap = job_ptr->details->req_node_bitmap;

	/* clear nodes from the bitmap that don't have available resources */
	for (i = 0; i < cr_node_cnt; i++) {
		if (!bit_test(node_map, i))
			continue;
		/* Make sure we don't say we can use a node exclusively
		 * that is bigger than our max cpu count. */
		if (((!job_ptr->details->shared) &&
		     (job_ptr->details->max_cpus != NO_VAL) &&
		     (job_ptr->details->max_cpus < cpu_cnt[i])) ||
		/* OR node has no CPUs */
		    (cpu_cnt[i] < 1)) {
			if (reqmap && bit_test(reqmap, i)) {
				/* can't clear a required node! */
				return SLURM_ERROR;
			}
			bit_clear(node_map, i);
		}
	}

	origmap = bit_copy(node_map);
	if (origmap == NULL)
		fatal("bit_copy malloc failure");

	ec = _eval_nodes(job_ptr, node_map, cr_node_cnt, cpu_cnt);

	if (ec == SLURM_SUCCESS) {
		FREE_NULL_BITMAP(origmap);
		return ec;
	}

	/* This nodeset didn't work. To avoid a possible knapsack problem,
	 * incrementally remove nodes with low cpu counts and retry */
	for (i = 0; i < cr_node_cnt; i++) {
		most_cpus = MAX(most_cpus, cpu_cnt[i]);
	}

	for (count = 1; count < most_cpus; count++) {
		int nochange = 1;
		bit_or(node_map, origmap);
		for (i = 0; i < cr_node_cnt; i++) {
			if ((cpu_cnt[i] > 0) && (cpu_cnt[i] <= count)) {
				if (!bit_test(node_map, i))
					continue;
				if (reqmap && bit_test(reqmap, i))
					continue;
				nochange = 0;
				bit_clear(node_map, i);
				bit_clear(origmap, i);
			}
		}
		if (nochange)
			continue;
		ec = _eval_nodes(job_ptr, node_map, cr_node_cnt, cpu_cnt);
		if (ec == SLURM_SUCCESS) {
			FREE_NULL_BITMAP(origmap);
			return ec;
		}
	}
	FREE_NULL_BITMAP(origmap);
	return ec;
}


/* Select the best set of resources for the given job
 * IN: job_ptr      - pointer to the job requesting resources
 * IN/OUT: node_map - bitmap of available nodes / bitmap of selected nodes
 * IN: cr_node_cnt  - total number of nodes in the cluster
 * IN/OUT: core_map - bitmap of available cores / bitmap of selected cores
 * IN: cr_type      - resource type
 * IN: test_only    - ignore allocated memory check
 * RET - array with number of CPUs available per node or NULL if not runnable
 */
static uint16_t *_select_nodes(struct job_record *job_ptr,
				bitstr_t *node_map, uint32_t cr_node_cnt,
				bitstr_t *core_map,
				struct node_use_record *node_usage,
				uint16_t cr_type, bool test_only)
{
	int rc;
	uint16_t *cpu_cnt, *cpus = NULL;
	uint32_t start, n, a;
	bitstr_t *req_map = job_ptr->details->req_node_bitmap;

	if (bit_set_count(node_map) == 0)
		return NULL;

	/* get resource usage for this job from each available node */
	_get_res_usage(job_ptr, node_map, core_map, cr_node_cnt,
		       node_usage, cr_type, &cpu_cnt, test_only);

	/* clear all nodes that do not have any
	 * usable resources for this job */
	for (n = 0; n < cr_node_cnt; n++) {
		if (bit_test(node_map, n) && (cpu_cnt[n] == 0)) {
			/* no resources are available for this node */
			if (req_map && bit_test(req_map, n)) {
				/* cannot clear a required node! */
				xfree(cpu_cnt);
				return NULL;
			}
			bit_clear(node_map, n);
		}
	}
	if (bit_set_count(node_map) == 0) {
		xfree(cpu_cnt);
		return NULL;
	}

	/* choose the best nodes for the job */
	rc = _choose_nodes(job_ptr, node_map, cr_node_cnt, cpu_cnt);

	/* if successful, sync up the core_map with the node_map, and
	 * create a cpus array */
	if (rc == SLURM_SUCCESS) {
		cpus = xmalloc(bit_set_count(node_map) * sizeof(uint16_t));
		start = 0;
		a = 0;
		for (n = 0; n < cr_node_cnt; n++) {
			if (bit_test(node_map, n)) {
				cpus[a++] = cpu_cnt[n];
				if (cr_get_coremap_offset(n) != start) {
					bit_nclear(core_map, start,
						   (cr_get_coremap_offset(n))-1);
				}
				start = cr_get_coremap_offset(n + 1);
			}
		}
		if (cr_get_coremap_offset(n) != start) {
			bit_nclear(core_map, start, cr_get_coremap_offset(n)-1);
		}
	}

	xfree(cpu_cnt);
	return cpus;
}


/* cr_job_test - does most of the real work for select_p_job_test(), which
 *	includes contiguous selection, load-leveling and max_share logic
 *
 * PROCEDURE:
 *
 * Step 1: compare nodes in "avail" bitmap with current node state data
 *         to find available nodes that match the job request
 *
 * Step 2: check resources in "avail" bitmap with allocated resources from
 *         higher priority partitions (busy resources are UNavailable)
 *
 * Step 3: select resource usage on remaining resources in "avail" bitmap
 *         for this job, with the placement influenced by existing
 *         allocations
 */
extern int cr_job_test(struct job_record *job_ptr, bitstr_t *bitmap, int mode,
			uint16_t cr_type, enum node_cr_state job_node_req, 
			uint32_t cr_node_cnt,
			struct part_res_record *cr_part_ptr,
			struct node_use_record *node_usage)
{
	int error_code = SLURM_SUCCESS, ll; /* ll = layout array index */
	uint16_t *layout_ptr = NULL;
	bitstr_t *orig_map, *avail_cores, *free_cores;
	bitstr_t *tmpcore = NULL, *reqmap = NULL;
	bool test_only;
	uint32_t c, i, k, n, csize, total_cpus, save_mem = 0;
	int32_t build_cnt;
	job_resources_t *job_res;
	struct job_details *details_ptr;
	struct part_res_record *p_ptr, *jp_ptr;
	uint16_t *cpu_count;

	details_ptr = job_ptr->details;
	layout_ptr  = details_ptr->req_node_layout;
	reqmap      = details_ptr->req_node_bitmap;

	free_job_resources(&job_ptr->job_resrcs);

	if (mode == SELECT_MODE_TEST_ONLY)
		test_only = true;
	else	/* SELECT_MODE_RUN_NOW || SELECT_MODE_WILL_RUN  */
		test_only = false;

	/* check node_state and update the node bitmap as necessary */
	if (!test_only) {
		error_code = _verify_node_state(cr_part_ptr, job_ptr,
						bitmap, cr_type, node_usage,
						job_node_req);
		if (error_code != SLURM_SUCCESS) {
			return error_code;
		}
	}

	if (select_debug_flags & DEBUG_FLAG_CPU_BIND) {
		info("cons_res: cr_job_test: evaluating job %u on %u nodes",
		     job_ptr->job_id, bit_set_count(bitmap));
	}

	orig_map = bit_copy(bitmap);
	avail_cores = _make_core_bitmap(bitmap);

	/* test to make sure that this job can succeed with all avail_cores
	 * if 'no' then return FAIL
	 * if 'yes' then we will seek the optimal placement for this job
	 *          within avail_cores
	 */
	free_cores = bit_copy(avail_cores);
	cpu_count = _select_nodes(job_ptr, bitmap, cr_node_cnt, free_cores,
				  node_usage, cr_type, test_only);
	if (cpu_count == NULL) {
		/* job cannot fit */
		FREE_NULL_BITMAP(orig_map);
		FREE_NULL_BITMAP(free_cores);
		FREE_NULL_BITMAP(avail_cores);
		if (select_debug_flags & DEBUG_FLAG_CPU_BIND) {
			info("cons_res: cr_job_test: test 0 fail: "
			     "insufficient resources");
		}
		return SLURM_ERROR;
	} else if (test_only) {
		FREE_NULL_BITMAP(orig_map);
		FREE_NULL_BITMAP(free_cores);
		FREE_NULL_BITMAP(avail_cores);
		xfree(cpu_count);
		if (select_debug_flags & DEBUG_FLAG_CPU_BIND)
			info("cons_res: cr_job_test: test 0 pass: test_only");
		return SLURM_SUCCESS;
	} else if (!job_ptr->best_switch) {
		FREE_NULL_BITMAP(orig_map);
		FREE_NULL_BITMAP(free_cores);
		FREE_NULL_BITMAP(avail_cores);
		xfree(cpu_count);
		return SLURM_ERROR;
	}
	if (cr_type == CR_MEMORY) {
		/* CR_MEMORY does not care about existing CPU allocations,
		 * so we can jump right to job allocation from here */
		goto alloc_job;
	}
	xfree(cpu_count);
	if (select_debug_flags & DEBUG_FLAG_CPU_BIND) {
		info("cons_res: cr_job_test: test 0 pass - "
		     "job fits on given resources");
	}

	/* now that we know that this job can run with the given resources,
	 * let's factor in the existing allocations and seek the optimal set
	 * of resources for this job. Here is the procedure:
	 *
	 * Step 1: Seek idle CPUs across all partitions. If successful then
	 *         place job and exit. If not successful, then continue. Two
	 *         related items to note:
	 *          1. Jobs that don't share CPUs finish with step 1.
	 *          2. The remaining steps assume sharing or preemption.
	 *
	 * Step 2: Remove resources that are in use by higher-priority
	 *         partitions, and test that job can still succeed. If not
	 *         then exit.
	 *
	 * Step 3: Seek idle nodes among the partitions with the same
	 *         priority as the job's partition. If successful then
	 *         goto Step 6. If not then continue:
	 *
	 * Step 4: Seek placement within the job's partition. Search
	 *         row-by-row. If no placement if found, then exit. If a row
	 *         is found, then continue:
	 *
	 * Step 5: Place job and exit. FIXME! Here is where we need a
	 *         placement algorithm that recognizes existing job
	 *         boundaries and tries to "overlap jobs" as efficiently
	 *         as possible.
	 *
	 * Step 6: Place job and exit. FIXME! here is we use a placement
	 *         algorithm similar to Step 5 on jobs from lower-priority
	 *         partitions.
	 */


	/*** Step 1 ***/
	bit_copybits(bitmap, orig_map);
	bit_copybits(free_cores, avail_cores);

	/* remove all existing allocations from free_cores */
	tmpcore = bit_copy(free_cores);
	for (p_ptr = cr_part_ptr; p_ptr; p_ptr = p_ptr->next) {
		if (!p_ptr->row)
			continue;
		for (i = 0; i < p_ptr->num_rows; i++) {
			if (!p_ptr->row[i].row_bitmap)
				continue;
			bit_copybits(tmpcore, p_ptr->row[i].row_bitmap);
			bit_not(tmpcore); /* set bits now "free" resources */
			bit_and(free_cores, tmpcore);
		}
	}
	cpu_count = _select_nodes(job_ptr, bitmap, cr_node_cnt, free_cores,
				  node_usage, cr_type, test_only);
	if ((cpu_count) && (job_ptr->best_switch)) {
		/* job fits! We're done. */
		if (select_debug_flags & DEBUG_FLAG_CPU_BIND) {
			info("cons_res: cr_job_test: test 1 pass - "
			     "idle resources found");
		}
		goto alloc_job;
	}

	if (job_node_req == NODE_CR_ONE_ROW) {
		/* This job CANNOT share CPUs regardless of priority,
		 * so we fail here. Note that Shared=EXCLUSIVE was already
		 * addressed in _verify_node_state() and job preemption
		 * removes jobs from simulated resource allocation map
		 * before this point. */
		if (select_debug_flags & DEBUG_FLAG_CPU_BIND) {
			info("cons_res: cr_job_test: test 1 fail - "
			     "no idle resources available");
		}
		goto alloc_job;
	}
	if (select_debug_flags & DEBUG_FLAG_CPU_BIND) {
		info("cons_res: cr_job_test: test 1 fail - "
		     "not enough idle resources");
	}

	/*** Step 2 ***/
	bit_copybits(bitmap, orig_map);
	bit_copybits(free_cores, avail_cores);

	for (jp_ptr = cr_part_ptr; jp_ptr; jp_ptr = jp_ptr->next) {
		if (jp_ptr->part_ptr == job_ptr->part_ptr)
			break;
	}
	if (!jp_ptr) {
		fatal("cons_res error: could not find partition for job %u",
			job_ptr->job_id);
	}

	/* remove existing allocations (jobs) from higher-priority partitions
	 * from avail_cores */
	for (p_ptr = cr_part_ptr; p_ptr; p_ptr = p_ptr->next) {
		if (p_ptr->part_ptr->priority <= jp_ptr->part_ptr->priority)
			continue;
		if (!p_ptr->row)
			continue;
		for (i = 0; i < p_ptr->num_rows; i++) {
			if (!p_ptr->row[i].row_bitmap)
				continue;
			bit_copybits(tmpcore, p_ptr->row[i].row_bitmap);
			bit_not(tmpcore); /* set bits now "free" resources */
			bit_and(free_cores, tmpcore);
		}
	}
	/* make these changes permanent */
	bit_copybits(avail_cores, free_cores);
	cpu_count = _select_nodes(job_ptr, bitmap, cr_node_cnt, free_cores,
				  node_usage, cr_type, test_only);
	if (!cpu_count) {
		/* job needs resources that are currently in use by
		 * higher-priority jobs, so fail for now */
		if (select_debug_flags & DEBUG_FLAG_CPU_BIND) {
			info("cons_res: cr_job_test: test 2 fail - "
			     "resources busy with higher priority jobs");
		}
		goto alloc_job;
	}
	xfree(cpu_count);
	if (select_debug_flags & DEBUG_FLAG_CPU_BIND) {
		info("cons_res: cr_job_test: test 2 pass - "
		     "available resources for this priority");
	}

	/*** Step 3 ***/
	bit_copybits(bitmap, orig_map);
	bit_copybits(free_cores, avail_cores);

	/* remove existing allocations (jobs) from same-priority partitions
	 * from avail_cores */
	for (p_ptr = cr_part_ptr; p_ptr; p_ptr = p_ptr->next) {
		if (p_ptr->part_ptr->priority != jp_ptr->part_ptr->priority)
			continue;
		if (!p_ptr->row)
			continue;
		for (i = 0; i < p_ptr->num_rows; i++) {
			if (!p_ptr->row[i].row_bitmap)
				continue;
			bit_copybits(tmpcore, p_ptr->row[i].row_bitmap);
			bit_not(tmpcore); /* set bits now "free" resources */
			bit_and(free_cores, tmpcore);
		}
	}
	cpu_count = _select_nodes(job_ptr, bitmap, cr_node_cnt, free_cores,
				  node_usage, cr_type, test_only);
	if (cpu_count) {
		/* jobs from low-priority partitions are the only thing left
		 * in our way. for now we'll ignore them, but FIXME: we need
		 * a good placement algorithm here that optimizes "job overlap"
		 * between this job (in these idle nodes) and the low-priority
		 * jobs */
		if (select_debug_flags & DEBUG_FLAG_CPU_BIND) {
			info("cons_res: cr_job_test: test 3 pass - "
			     "found resources");
		}
		goto alloc_job;
	}
	if (select_debug_flags & DEBUG_FLAG_CPU_BIND) {
		info("cons_res: cr_job_test: test 3 fail - "
		     "not enough idle resources in same priority");
	}


	/*** Step 4 ***/
	/* try to fit the job into an existing row
	 *
	 * tmpcore = worker core_bitmap
	 * free_cores = core_bitmap to be built
	 * avail_cores = static core_bitmap of all available cores
	 */

	if (jp_ptr->row == NULL) {
		/* there's no existing jobs in this partition, so place
		 * the job in avail_cores. FIXME: still need a good
		 * placement algorithm here that optimizes "job overlap"
		 * between this job (in these idle nodes) and existing
		 * jobs in the other partitions with <= priority to
		 * this partition */
		bit_copybits(bitmap, orig_map);
		bit_copybits(free_cores, avail_cores);
		cpu_count = _select_nodes(job_ptr, bitmap, cr_node_cnt,
					  free_cores, node_usage, cr_type,
					  test_only);
		if (select_debug_flags & DEBUG_FLAG_CPU_BIND) {
			info("cons_res: cr_job_test: test 4 pass - "
			     "first row found");
		}
		goto alloc_job;
	}

	cr_sort_part_rows(jp_ptr);
	c = jp_ptr->num_rows;
	if (job_node_req != NODE_CR_AVAILABLE)
		c = 1;
	for (i = 0; i < c; i++) {
		if (!jp_ptr->row[i].row_bitmap)
			break;
		bit_copybits(bitmap, orig_map);
		bit_copybits(free_cores, avail_cores);
		bit_copybits(tmpcore, jp_ptr->row[i].row_bitmap);
		bit_not(tmpcore);
		bit_and(free_cores, tmpcore);
		cpu_count = _select_nodes(job_ptr, bitmap, cr_node_cnt,
					  free_cores, node_usage, cr_type,
					  test_only);
		if (cpu_count) {
			if (select_debug_flags & DEBUG_FLAG_CPU_BIND) {
				info("cons_res: cr_job_test: test 4 pass - "
				     "row %i", i);
			}
			break;
		}
		if (select_debug_flags & DEBUG_FLAG_CPU_BIND)
			info("cons_res: cr_job_test: test 4 fail - row %i", i);
	}

	if ((i < c) && !jp_ptr->row[i].row_bitmap) {
		/* we've found an empty row, so use it */
		bit_copybits(bitmap, orig_map);
		bit_copybits(free_cores, avail_cores);
		if (select_debug_flags & DEBUG_FLAG_CPU_BIND) {
			info("cons_res: cr_job_test: "
			     "test 4 trying empty row %i",i);
		}
		cpu_count = _select_nodes(job_ptr, bitmap, cr_node_cnt,
					  free_cores, node_usage, cr_type,
					  test_only);
	}

	if (!cpu_count) {
		/* job can't fit into any row, so exit */
		if (select_debug_flags & DEBUG_FLAG_CPU_BIND) {
			info("cons_res: cr_job_test: test 4 fail - "
			     "busy partition");
		}
		goto alloc_job;

	}

	/*** CONSTRUCTION ZONE FOR STEPs 5 AND 6 ***
	 * Note that while the job may have fit into a row, it should
	 * still be run through a good placement algorithm here that
	 * optimizes "job overlap" between this job (in these idle nodes)
	 * and existing jobs in the other partitions with <= priority to
	 * this partition */

alloc_job:
	/* at this point we've found a good set of
	 * bits to allocate to this job:
	 * - bitmap is the set of nodes to allocate
	 * - free_cores is the set of allocated cores
	 * - cpu_count is the number of cpus per allocated node
	 *
	 * Next steps are to cleanup the worker variables,
	 * create the job_resources struct,
	 * distribute the job on the bits, and exit
	 */
	FREE_NULL_BITMAP(orig_map);
	FREE_NULL_BITMAP(avail_cores);
	FREE_NULL_BITMAP(tmpcore);
	if ((!cpu_count) || (!job_ptr->best_switch)) {
		/* we were sent here to cleanup and exit */
		FREE_NULL_BITMAP(free_cores);
		if (select_debug_flags & DEBUG_FLAG_CPU_BIND) {
			info("cons_res: exiting cr_job_test with no "
			     "allocation");
		}
		return SLURM_ERROR;
	}

	/* At this point we have:
	 * - a bitmap of selected nodes
	 * - a free_cores bitmap of usable cores on each selected node
	 * - a per-alloc-node cpu_count array
	 */

	if ((mode != SELECT_MODE_WILL_RUN) && (job_ptr->part_ptr == NULL))
		error_code = EINVAL;
	if ((error_code == SLURM_SUCCESS) && (mode == SELECT_MODE_WILL_RUN))
		job_ptr->total_cpus = 1;
	if ((error_code != SLURM_SUCCESS) || (mode != SELECT_MODE_RUN_NOW)) {
		FREE_NULL_BITMAP(free_cores);
		xfree(cpu_count);
		return error_code;
	}

	if (select_debug_flags & DEBUG_FLAG_CPU_BIND) {
		info("cons_res: cr_job_test: distributing job %u",
		     job_ptr->job_id);
	}

	/** create the struct_job_res  **/
	job_res                   = create_job_resources();
	job_res->node_bitmap      = bit_copy(bitmap);
	job_res->nodes            = bitmap2node_name(bitmap);
	if (job_res->node_bitmap == NULL)
		fatal("bit_copy malloc failure");
	job_res->nhosts           = bit_set_count(bitmap);
	job_res->ncpus            = job_res->nhosts;
	if (job_ptr->details->ntasks_per_node)
		job_res->ncpus   *= details_ptr->ntasks_per_node;
	job_res->ncpus            = MAX(job_res->ncpus,
					details_ptr->min_cpus);
	job_res->ncpus            = MAX(job_res->ncpus,
					details_ptr->pn_min_cpus);
	job_res->node_req         = job_node_req;
	job_res->cpus             = cpu_count;
	job_res->cpus_used        = xmalloc(job_res->nhosts *
					    sizeof(uint16_t));
	job_res->memory_allocated = xmalloc(job_res->nhosts *
					    sizeof(uint32_t));
	job_res->memory_used      = xmalloc(job_res->nhosts *
					    sizeof(uint32_t));

	/* store the hardware data for the selected nodes */
	error_code = build_job_resources(job_res, node_record_table_ptr,
					  select_fast_schedule);
	if (error_code != SLURM_SUCCESS) {
		free_job_resources(&job_res);
		FREE_NULL_BITMAP(free_cores);
		return error_code;
	}

	/* sync up cpus with layout_ptr, total up
	 * all cpus, and load the core_bitmap */
	ll = -1;
	total_cpus = 0;
	c = 0;
	csize = bit_size(job_res->core_bitmap);
	for (i = 0, n = 0; n < cr_node_cnt; n++) {
		uint32_t j;
		if (layout_ptr && reqmap && bit_test(reqmap,n))
			ll++;
		if (bit_test(bitmap, n) == 0)
			continue;
		j = cr_get_coremap_offset(n);
		k = cr_get_coremap_offset(n + 1);
		for (; j < k; j++, c++) {
			if (bit_test(free_cores, j)) {
				if (c >= csize)	{
					error("cons_res: cr_job_test "
					      "core_bitmap index error on "
					      "node %s", 
					      select_node_record[n].node_ptr->
					      name);
					drain_nodes(select_node_record[n].
						    node_ptr->name,
						    "Bad core count",
						    getuid());
					free_job_resources(&job_res);
					FREE_NULL_BITMAP(free_cores);
					return SLURM_ERROR;
				}
				bit_set(job_res->core_bitmap, c);
			}
		}

		if (layout_ptr && reqmap && bit_test(reqmap, n)) {
			job_res->cpus[i] = MIN(job_res->cpus[i],
					       layout_ptr[ll]);
		} else if (layout_ptr) {
			job_res->cpus[i] = 0;
		}
		total_cpus += job_res->cpus[i];
		i++;
	}

	/* When 'srun --overcommit' is used, ncpus is set to a minimum value
	 * in order to allocate the appropriate number of nodes based on the
	 * job request.
	 * For cons_res, all available logical processors will be allocated on
	 * each allocated node in order to accommodate the overcommit request.
	 */
	if (details_ptr->overcommit && details_ptr->num_tasks)
		job_res->ncpus = MIN(total_cpus, details_ptr->num_tasks);

	if (select_debug_flags & DEBUG_FLAG_CPU_BIND) {
		info("cons_res: cr_job_test: job %u ncpus %u cbits "
		     "%u/%u nbits %u", job_ptr->job_id,
		     job_res->ncpus, bit_set_count(free_cores),
		     bit_set_count(job_res->core_bitmap), job_res->nhosts);
	}
	FREE_NULL_BITMAP(free_cores);

	/* distribute the tasks and clear any unused cores */
	job_ptr->job_resrcs = job_res;
	error_code = cr_dist(job_ptr, cr_type);
	if (error_code != SLURM_SUCCESS) {
		free_job_resources(&job_ptr->job_resrcs);
		return error_code;
	}

	/* translate job_res->cpus array into format with rep count */
	build_cnt = build_job_resources_cpu_array(job_res);
	if (build_cnt >= 0)
		job_ptr->total_cpus = build_cnt;
	else
		job_ptr->total_cpus = total_cpus;	/* best guess */

	if (!(cr_type & CR_MEMORY))
		return error_code;

	/* load memory allocated array */
	save_mem =details_ptr->pn_min_memory;
	if (save_mem & MEM_PER_CPU) {
		/* memory is per-cpu */
		save_mem &= (~MEM_PER_CPU);
		for (i = 0; i < job_res->nhosts; i++) {
			job_res->memory_allocated[i] = job_res->cpus[i] *
						       save_mem;
		}
	} else {
		/* memory is per-node */
		for (i = 0; i < job_res->nhosts; i++) {
			job_res->memory_allocated[i] = save_mem;
		}
	}
	return error_code;
}
