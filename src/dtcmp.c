/* Copyright (c) 2012, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * Written by Adam Moody <moody20@llnl.gov> and Edgar A. Leon <leon@llnl.gov>.
 * LLNL-CODE-557516.
 * All rights reserved.
 * This file is part of the DTCMP library.
 * For details, see https://github.com/hpc/dtcmp
 * Please also read this file: LICENSE.TXT. */

#include <stdlib.h>
#include "mpi.h"
#include "dtcmp_internal.h"
#include "dtcmp_ops.h"

/* set up our DTCMP_IN_PLACE constant
 * (just a void* pointer to an int in static memory) */
static int DTCMP_IN_PLACE_LOCATION;
const void* DTCMP_IN_PLACE = (const void*) &DTCMP_IN_PLACE_LOCATION;

/* we'll dup comm_self during init, which we need for our memcpy */
MPI_Comm dtcmp_comm_self = MPI_COMM_NULL;

/* define our NULL comparison handle */
DTCMP_Op DTCMP_OP_NULL = NULL;

/* set predefined comparison ops to NULL now,
 * we'll fill these in during init */
DTCMP_Op DTCMP_OP_INT_ASCEND  = NULL;
DTCMP_Op DTCMP_OP_INT_DESCEND = NULL;

/* determine whether type is contiguous, has a true lower bound of 0,
 * and extent == true_extent */
static int dtcmp_type_is_valid(MPI_Datatype type)
{
  /* get (user-defined) lower bound and extent */
  MPI_Aint lb, extent;
  MPI_Type_get_extent(type, &lb, &extent);

  /* get true lower bound and extent */
  MPI_Aint true_lb, true_extent;
  MPI_Type_get_true_extent(type, &true_lb, &true_extent);

  /* get size of type */
  int size;
  MPI_Type_size(type, &size);

  /* check that type is contiguous (size == true_extent ==> no holes) */
  if (size != true_extent) {
    return 0;
  }

  /* check that lower bounds are 0 */
  if (lb != 0 || true_lb != 0) {
    return 0;
  }

  /* check that extent == true_extent ==> no funny business if we
   * concatenate a series of these types */
  if (extent != true_extent) {
    return 0;
  }

  /* check that extent is positive */
  if (extent <= 0) {
    return 0;
  }

  return 1;
}

/* initialize the sorting library */
int DTCMP_Init()
{
  /* copy comm_self */
  MPI_Comm_dup(MPI_COMM_SELF, &dtcmp_comm_self);

  /* setup predefined cmp handles */
  DTCMP_Op_create(MPI_INT, dtcmp_op_fn_int_ascend,  &DTCMP_OP_INT_ASCEND);
  DTCMP_Op_create(MPI_INT, dtcmp_op_fn_int_descend, &DTCMP_OP_INT_DESCEND);

  return DTCMP_SUCCESS;
}

/* finalize the sorting library and set static values back
 * to their pre-init state */
int DTCMP_Finalize()
{
  /* free off predefined cmp handles */
  DTCMP_Op_free(&DTCMP_OP_INT_DESCEND);
  DTCMP_Op_free(&DTCMP_OP_INT_ASCEND);

  /* free our copy of comm_self */
  if (dtcmp_comm_self != MPI_COMM_NULL) {
    MPI_Comm_free(&dtcmp_comm_self);
    dtcmp_comm_self = MPI_COMM_NULL;
  }

  return DTCMP_SUCCESS;
}

/* create a user-defined comparison operation, associate compare function
 * pointer and datatype of key */
int DTCMP_Op_create(MPI_Datatype key, DTCMP_Op_fn fn, DTCMP_Op* cmp)
{
  /* check parameters */
  if (cmp == NULL) {
    return DTCMP_FAILURE;
  }

  /* allocate a handle and fill in its type and function pointer */
  dtcmp_op_init(DTCMP_OP_TYPE_BASIC, key, fn, DTCMP_OP_NULL, cmp);

  return DTCMP_SUCCESS;
}

/* create a series comparison which executes the first comparison operation
 * and then the second if the first evaluates to equal */
int DTCMP_Op_create_series(DTCMP_Op first, DTCMP_Op second, DTCMP_Op* cmp)
{
  /* check parameters */
  if (cmp == NULL) {
    return DTCMP_FAILURE;
  }

  /* make a full copy of the second compare operation */
  DTCMP_Op copy;
  dtcmp_op_copy(&copy, second);

  /* now build a new comparison type using the key and fn of the first and add in second */
  DTCMP_Handle_t* c = (DTCMP_Handle_t*) first;
  dtcmp_op_init(DTCMP_OP_TYPE_SERIES, c->key, c->fn, copy, cmp);

  return DTCMP_SUCCESS;
}

/* create a series comparison which executes the first comparison operation
 * and then the second if the first evaluates to equal */
int DTCMP_Op_create_hseries(DTCMP_Op first, MPI_Aint disp, DTCMP_Op second, DTCMP_Op* cmp)
{
  /* check parameters */
  if (cmp == NULL) {
    return DTCMP_FAILURE;
  }

  /* make a full copy of the second compare operation */
  DTCMP_Op copy;
  dtcmp_op_copy(&copy, second);

  /* now build a new comparison type using the key and fn of the first and add in second */
  DTCMP_Handle_t* c = (DTCMP_Handle_t*) first;
  dtcmp_op_hinit(DTCMP_OP_TYPE_SERIES, c->key, c->fn, disp, copy, cmp);

  return DTCMP_SUCCESS;
}

/* free object referenced by comparison operation handle */
int DTCMP_Op_free(DTCMP_Op* cmp)
{
  if (cmp != NULL && *cmp != DTCMP_OP_NULL) {
    DTCMP_Handle_t* c = (DTCMP_Handle_t*)(*cmp);
    MPI_Type_free(&(c->key));
    if (c->series != DTCMP_OP_NULL) {
      DTCMP_Op_free(&(c->series));
    }
    free(*cmp);
    *cmp = DTCMP_OP_NULL;
    return DTCMP_SUCCESS;
  } else {
    return DTCMP_FAILURE;
  }
}

/* TODO: turn this into a macro */
/* copy memory from srcbuf to dstbuf using committed MPI datatypes */
int DTCMP_Memcpy(
  void* dstbuf,       int dstcount, MPI_Datatype dsttype,
  const void* srcbuf, int srccount, MPI_Datatype srctype)
{
  /* execute sendrecv to ourself on comm_self */
  MPI_Sendrecv(
    (void*)srcbuf, srccount, srctype, 0, 999,
    dstbuf,        dstcount, dsttype, 0, 999,
    dtcmp_comm_self, MPI_STATUS_IGNORE
  );

  return DTCMP_SUCCESS;
}

int DTCMP_Search_low_combined(
  const void* target,
  const void* list,
  int low,
  int high,
  MPI_Datatype key,
  MPI_Datatype keysat,
  DTCMP_Op cmp,
  int* flag,
  int* index)
{
  /* check parameters */
  if (target == NULL || flag == NULL || list == NULL) {
    return DTCMP_FAILURE;
  }
  if (!dtcmp_type_is_valid(key)) {
    return DTCMP_FAILURE;
  }
  if (!dtcmp_type_is_valid(keysat)) {
    return DTCMP_FAILURE;
  }

  return DTCMP_Search_low_combined_binary(target, list, low, high, key, keysat, cmp, flag, index);
}

int DTCMP_Search_high_combined(
  const void* target,
  const void* list,
  int low,
  int high,
  MPI_Datatype key,
  MPI_Datatype keysat,
  DTCMP_Op cmp,
  int* flag,
  int* index)
{
  /* check parameters */
  if (target == NULL || flag == NULL || list == NULL) {
    return DTCMP_FAILURE;
  }
  if (!dtcmp_type_is_valid(key)) {
    return DTCMP_FAILURE;
  }
  if (!dtcmp_type_is_valid(keysat)) {
    return DTCMP_FAILURE;
  }

  return DTCMP_Search_high_combined_binary(target, list, low, high, key, keysat, cmp, flag, index);
}

int DTCMP_Search_low_list_combined(
  int num,
  const void* targets,
  const void* list,
  int low,
  int high,
  MPI_Datatype key,
  MPI_Datatype keysat,
  DTCMP_Op cmp,
  int* indicies)
{
  /* check parameters */
  if (num < 0) {
    return DTCMP_FAILURE;
  }
  if (num > 0 && (targets == NULL || indicies == NULL)) {
    return DTCMP_FAILURE;
  }
  if (!dtcmp_type_is_valid(key)) {
    return DTCMP_FAILURE;
  }
  if (!dtcmp_type_is_valid(keysat)) {
    return DTCMP_FAILURE;
  }

  return DTCMP_Search_low_list_combined_binary(num, targets, list, low, high, key, keysat, cmp, indicies);
}

#if 0
int DTCMP_Partition_combined(
  int inpivot,
  const void* inbuf,
  void* outbuf,
  int* outpivot,
  int count,
  MPI_Datatype key,
  MPI_Datatype keysat,
  DTCMP_Op cmp)
{
  /* check parameters */
  if (count < 0) {
    return DTCMP_FAILURE;
  }
  if (count > 0 && outbuf == NULL) {
    return DTCMP_FAILURE;
  }
  if (!dtcmp_type_is_valid(key)) {
    return DTCMP_FAILURE;
  }
  if (!dtcmp_type_is_valid(keysat)) {
    return DTCMP_FAILURE;
  }

  return DTCMP_Partition_combined_equal(inpivot, inbuf, outbuf, low, high, key, keysat, cmp, indicies);
}
#endif

int DTCMP_Merge_combined(
  int num,
  const void* inbufs[],
  int counts[],
  void* outbuf,
  MPI_Datatype key,
  MPI_Datatype keysat,
  DTCMP_Op cmp)
{
  /* check parameters */
  if (num < 2) {
    return DTCMP_FAILURE;
  }
  if (num > 0 && (inbufs == NULL || counts == NULL || outbuf == NULL)) {
    return DTCMP_FAILURE;
  }
  if (!dtcmp_type_is_valid(key)) {
    return DTCMP_FAILURE;
  }
  if (!dtcmp_type_is_valid(keysat)) {
    return DTCMP_FAILURE;
  }

  if (num == 2) {
    /* O(N) time */
    return DTCMP_Merge_combined_2way(num, inbufs, counts, outbuf, key, keysat, cmp);
  } else {
    /* O(log(num) * N) time */
    return DTCMP_Merge_combined_kway_heap(num, inbufs, counts, outbuf, key, keysat, cmp);
  }
}

/* execute a purely local sort */
int DTCMP_Sort_local_combined(
  const void* inbuf, 
  void* outbuf,
  int count,
  MPI_Datatype key,
  MPI_Datatype keysat,
  DTCMP_Op cmp)
{
  /* check parameters */
  if (count < 0) {
    return DTCMP_FAILURE;
  }
  if (count > 0 && outbuf == NULL) {
    return DTCMP_FAILURE;
  }
  if (!dtcmp_type_is_valid(key)) {
    return DTCMP_FAILURE;
  }
  if (!dtcmp_type_is_valid(keysat)) {
    return DTCMP_FAILURE;
  }

  return DTCMP_Sort_local_combined_randquicksort(inbuf, outbuf, count, key, keysat, cmp);
  return DTCMP_Sort_local_combined_insertionsort(inbuf, outbuf, count, key, keysat, cmp);
  return DTCMP_Sort_local_combined_mergesort(inbuf, outbuf, count, key, keysat, cmp);

  /* if keysat is valid type and if function is basic, we can just call qsort */
  DTCMP_Handle_t* c = (DTCMP_Handle_t*) cmp;
  if (c->type == DTCMP_OP_TYPE_BASIC) {
    return DTCMP_Sort_local_combined_qsort(inbuf, outbuf, count, key, keysat, cmp);
  }
}

int DTCMP_Sort_combined(
  const void* inbuf,
  void* outbuf,
  int count,
  MPI_Datatype key,
  MPI_Datatype keysat,
  DTCMP_Op cmp,
  MPI_Comm comm)
{
  /* check parameters */
  if (count < 0) {
    return DTCMP_FAILURE;
  }
  if (count > 0 && outbuf == NULL) {
    return DTCMP_FAILURE;
  }
  if (!dtcmp_type_is_valid(key)) {
    return DTCMP_FAILURE;
  }
  if (!dtcmp_type_is_valid(keysat)) {
    return DTCMP_FAILURE;
  }

  return DTCMP_Sort_combined_allgather(inbuf, outbuf, count, key, keysat, cmp, comm);
}

#define MMS_MIN (0)
#define MMS_MAX (1)
#define MMS_SUM (2)
static void min_max_sum(void* invec, void* inoutvec, int* len, MPI_Datatype* type)
{
   uint64_t* a = (uint64_t*) invec;
   uint64_t* b = (uint64_t*) inoutvec;

   int i;
   for (i = 0; i < *len; i++) {
     /* compute minimum across all ranks */
     if (a[MMS_MIN] < b[MMS_MIN]) {
       b[MMS_MIN] = a[MMS_MIN];
     }

     /* compute maximum across all ranks */
     if (a[MMS_MAX] > b[MMS_MAX]) {
       b[MMS_MAX] = a[MMS_MAX];
     }

     /* compute sum across all ranks */
     b[MMS_SUM] += a[MMS_SUM];

     /* advance to next element */
     a += 3;
     b += 3;
  }
}

int DTCMP_Sortv_combined(
  const void* inbuf,
  void* outbuf,
  int count,
  MPI_Datatype key,
  MPI_Datatype keysat,
  DTCMP_Op cmp,
  MPI_Comm comm)
{
  /* check parameters */
  if (count < 0) {
    return DTCMP_FAILURE;
  }
  if (count > 0 && outbuf == NULL) {
    return DTCMP_FAILURE;
  }
  if (!dtcmp_type_is_valid(key)) {
    return DTCMP_FAILURE;
  }
  if (!dtcmp_type_is_valid(keysat)) {
    return DTCMP_FAILURE;
  }

  /* TODO: is uint64_t large enough for this count,
   * need something that holds |int|^2? */

  /* compute min/max/sum of counts across processes */
  MPI_Datatype type_3uint64t;
  MPI_Type_contiguous(3, MPI_UINT64_T, &type_3uint64t);
  MPI_Type_commit(&type_3uint64t);

  /* just integer min/max/addition of non-negative values,
   * so assume this is commutative */
  MPI_Op op;
  MPI_Op_create(min_max_sum, 1, &op);

  uint64_t reduce3[3];
  reduce3[MMS_MIN] = count;
  reduce3[MMS_MAX] = count;
  reduce3[MMS_SUM] = count;
  uint64_t allreduce[3];
  MPI_Allreduce(reduce3, allreduce, 1, type_3uint64t, op, comm);

  /* free our user-defined op and our type */
  MPI_Op_free(&op);
  MPI_Type_free(&type_3uint64t);

  /* if min==max, then just invoke Sort() routine
   * if sum(counts) is small gather to one task */

  return DTCMP_Sortv_combined_sortgather_scatter(inbuf, outbuf, count, key, keysat, cmp, comm);
  return DTCMP_Sortv_combined_allgather(inbuf, outbuf, count, key, keysat, cmp, comm);
}

int DTCMP_Rankv_combined(
  int count,
  const void* buf,
  int* groups,
  int  group_id[],
  int  group_ranks[],
  int  group_rank[],
  MPI_Datatype key,
  MPI_Datatype keysat,
  DTCMP_Op cmp,
  MPI_Comm comm)
{
  /* check parameters */
  if (count < 0) {
    return DTCMP_FAILURE;
  }
  if (count > 0 && buf == NULL) {
    return DTCMP_FAILURE;
  }
  if (!dtcmp_type_is_valid(key)) {
    return DTCMP_FAILURE;
  }
  if (!dtcmp_type_is_valid(keysat)) {
    return DTCMP_FAILURE;
  }

  return DTCMP_Rankv_combined_sort(
    count, buf,
    groups, group_id, group_ranks, group_rank,
    key, keysat, cmp, comm
  );
}

int DTCMP_Rankv_strings(
  int count,
  const char* strings[],
  int* groups,
  int  group_id[],
  int  group_ranks[],
  int  group_rank[],
  MPI_Comm comm)
{
  /* check parameters */
  if (count < 0) {
    return DTCMP_FAILURE;
  }
  if (count > 0 && strings == NULL) {
    return DTCMP_FAILURE;
  }

  return DTCMP_Rankv_strings_sort(count, strings, groups, group_id, group_ranks, group_rank, comm);
}


