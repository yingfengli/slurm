#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include <src/api/slurm.h>
#include <testsuite/dejagnu.h>

/* main is used here for testing purposes only */
	int 
main (int argc, char *argv[])
{
	int error_code, job_count, max_jobs;
	job_desc_msg_t job_mesg;
	resource_allocation_response_msg_t resp_msg ;

	if (argc > 1) 
		max_jobs = atoi (argv[1]);
	else
		max_jobs = 1;

	slurm_init_job_desc_msg( &job_mesg );
	job_mesg. contiguous = 1;
	job_mesg. groups = ("students,employee\0");
	job_mesg. name = ("job01\0");
	job_mesg. partition_key = "1234";
	job_mesg. min_procs = 4;
	job_mesg. min_memory = 1024;
	job_mesg. min_tmp_disk = 2034;
	job_mesg. partition = "batch\0";
	job_mesg. priority = 100;
	job_mesg. req_nodes = "lx[3000-3003]\0";
	job_mesg. job_script = "/bin/hostname\0";
	job_mesg. shared = 0;
	job_mesg. time_limit = 200;
	job_mesg. num_procs = 1000;
	job_mesg. num_nodes = 400;
	job_mesg. user_id = 1500;


	error_code = slurm_allocate_resources ( &job_mesg , & resp_msg , true ); 
	if (error_code)
		printf ("allocate error %d\n", error_code);
	else {
		printf ("allocate nodes %s to job %u\n", resp_msg.node_list, job_mesg.job_id);
	}
	job_count = 1;

	for ( ; job_count <max_jobs;  job_count++) {
		slurm_init_job_desc_msg( &job_mesg );
		job_mesg. contiguous = 1;
		job_mesg. groups = ("students,employee\0");
		job_mesg. name = ("more.big\0");
		job_mesg. partition_key = "1234";
		job_mesg. min_procs = 4;
		job_mesg. min_memory = 1024;
		job_mesg. min_tmp_disk = 2034;
		job_mesg. partition = "batch\0";
		job_mesg. priority = 100;
		job_mesg. req_nodes = "lx[3000-3003]\0";
		job_mesg. job_script = "/bin/hostname\0";
		job_mesg. shared = 0;
		job_mesg. time_limit = 200;
		job_mesg. num_procs = 4000;
		job_mesg. user_id = 1500;

		error_code = slurm_allocate_resources ( &job_mesg , & resp_msg , true ); 
		if (error_code) {
			printf ("allocate error %d\n", error_code);
			break;
		}
		else {
			printf ("allocate nodes %s to job %u\n", resp_msg.node_list, job_mesg.job_id);
		}
	}

	for ( ; job_count <max_jobs;  job_count++) {
		slurm_init_job_desc_msg( &job_mesg );
		job_mesg. name = ("more.tiny\0");
		job_mesg. num_procs = 40;
 	        job_mesg. user_id = 1500;

		error_code = slurm_allocate_resources ( &job_mesg , & resp_msg , true ); 
		if (error_code) {
			printf ("allocate error %d\n", error_code);
			break;
		}
		else {
			printf ("allocate nodes %s to job %u\n", resp_msg.node_list,  job_mesg.job_id);
		}
	}

	for ( ; job_count <max_jobs;  job_count++) {
		slurm_init_job_desc_msg( &job_mesg );
		job_mesg. name = ("more.queue\0");
		job_mesg. num_procs = 40;
 	        job_mesg. user_id = 1500;

		error_code = slurm_allocate_resources ( &job_mesg , & resp_msg , false ); 
		if (error_code) {
			printf ("allocate error %d\n", error_code);
			break;
		}
		else {
			printf ("allocate nodes %s to job %u\n", resp_msg.node_list,  job_mesg.job_id);
		}
	}

	return (0);
}
