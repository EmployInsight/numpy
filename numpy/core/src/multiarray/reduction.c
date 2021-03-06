/*
 * This file implements generic methods for computing reductions on arrays.
 *
 * Written by Mark Wiebe (mwwiebe@gmail.com)
 * Copyright (c) 2011 by Enthought, Inc.
 *
 * See LICENSE.txt for the license.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#define NPY_NO_DEPRECATED_API NPY_API_VERSION
#define _MULTIARRAYMODULE
#include <numpy/arrayobject.h>

#include "npy_config.h"
#include "numpy/npy_3kcompat.h"

#include "lowlevel_strided_loops.h"
#include "na_mask.h"
#include "reduction.h"

/*
 * Allocates a result array for a reduction operation, with
 * dimensions matching 'arr' except set to 1 with 0 stride
 * whereever axis_flags is True. Dropping the reduction axes
 * from the result must be done later by the caller once the
 * computation is complete.
 *
 * This function never adds an NA mask to the allocated
 * result, that is the responsibility of the caller. It also
 * always allocates a base class ndarray.
 *
 * If 'dtype' isn't NULL, this function steals its reference.
 */
static PyArrayObject *
allocate_reduce_result(PyArrayObject *arr, npy_bool *axis_flags,
                        PyArray_Descr *dtype, int subok)
{
    npy_intp strides[NPY_MAXDIMS], stride;
    npy_intp shape[NPY_MAXDIMS], *arr_shape = PyArray_DIMS(arr);
    npy_stride_sort_item strideperm[NPY_MAXDIMS];
    int idim, ndim = PyArray_NDIM(arr);

    if (dtype == NULL) {
        dtype = PyArray_DTYPE(arr);
        Py_INCREF(dtype);
    }

    PyArray_CreateSortedStridePerm(PyArray_NDIM(arr), PyArray_SHAPE(arr),
                                    PyArray_STRIDES(arr), strideperm);

    /* Build the new strides and shape */
    stride = dtype->elsize;
    memcpy(shape, arr_shape, ndim * sizeof(shape[0]));
    for (idim = ndim-1; idim >= 0; --idim) {
        npy_intp i_perm = strideperm[idim].perm;
        if (axis_flags[i_perm]) {
            strides[i_perm] = 0;
            shape[i_perm] = 1;
        }
        else {
            strides[i_perm] = stride;
            stride *= shape[i_perm];
        }
    }

    /* Finally, allocate the array */
    return (PyArrayObject *)PyArray_NewFromDescr(
                                    subok ? Py_TYPE(arr) : &PyArray_Type,
                                    dtype, ndim, shape, strides,
                                    NULL, 0, subok ? (PyObject *)arr : NULL);
}

/*
 * Conforms an output parameter 'out' to have 'ndim' dimensions
 * with dimensions of size one added in the appropriate places
 * indicated by 'axis_flags'.
 *
 * The return value is a view into 'out'.
 */
static PyArrayObject *
conform_reduce_result(int ndim, npy_bool *axis_flags,
                    PyArrayObject *out, int keepdims, const char *funcname)
{
    npy_intp strides[NPY_MAXDIMS], shape[NPY_MAXDIMS];
    npy_intp *strides_out = PyArray_STRIDES(out);
    npy_intp *shape_out = PyArray_DIMS(out);
    int idim, idim_out, ndim_out = PyArray_NDIM(out);
    PyArray_Descr *dtype;
    PyArrayObject_fields *ret;

    /*
     * If the 'keepdims' parameter is true, do a simpler validation and
     * return a new reference to 'out'.
     */
    if (keepdims) {
        if (PyArray_NDIM(out) != ndim) {
            PyErr_Format(PyExc_ValueError,
                    "output parameter for reduction operation %s "
                    "has the wrong number of dimensions (must match "
                    "the operand's when keepdims=True)", funcname);
            return NULL;
        }

        for (idim = 0; idim < ndim; ++idim) {
            if (axis_flags[idim]) {
                if (shape_out[idim] != 1) {
                    PyErr_Format(PyExc_ValueError,
                            "output parameter for reduction operation %s "
                            "has a reduction dimension not equal to one "
                            "(required when keepdims=True)", funcname);
                    return NULL;
                }
            }
        }

        Py_INCREF(out);
        return out;
    }

    /* Construct the strides and shape */
    idim_out = 0;
    for (idim = 0; idim < ndim; ++idim) {
        if (axis_flags[idim]) {
            strides[idim] = 0;
            shape[idim] = 1;
        }
        else {
            if (idim_out >= ndim_out) {
                PyErr_Format(PyExc_ValueError,
                        "output parameter for reduction operation %s "
                        "does not have enough dimensions", funcname);
                return NULL;
            }
            strides[idim] = strides_out[idim_out];
            shape[idim] = shape_out[idim_out];
            ++idim_out;
        }
    }

    if (idim_out != ndim_out) {
        PyErr_Format(PyExc_ValueError,
                "output parameter for reduction operation %s "
                "has too many dimensions", funcname);
        return NULL;
    }

    /* Allocate the view */
    dtype = PyArray_DESCR(out);
    Py_INCREF(dtype);
    ret = (PyArrayObject_fields *)PyArray_NewFromDescr(&PyArray_Type,
                               dtype,
                               ndim, shape,
                               strides,
                               PyArray_DATA(out),
               PyArray_FLAGS(out) & ~(NPY_ARRAY_MASKNA|NPY_ARRAY_OWNMASKNA),
                               NULL);
    if (ret == NULL) {
        return NULL;
    }
    Py_INCREF(out);
    if (PyArray_SetBaseObject((PyArrayObject *)ret, (PyObject *)out) < 0) {
        Py_DECREF(ret);
        return NULL;
    }

    /* Take a view of the mask if it exists */
    if (PyArray_HASMASKNA(out)) {
        npy_intp *strides_ret = ret->maskna_strides;
        strides_out = PyArray_MASKNA_STRIDES(out);
        idim_out = 0;
        for (idim = 0; idim < ndim; ++idim) {
            if (axis_flags[idim]) {
                strides_ret[idim] = 0;
            }
            else {
                strides_ret[idim] = strides_out[idim_out];
                ++idim_out;
            }
        }

        ret->maskna_dtype = PyArray_MASKNA_DTYPE(out);
        Py_INCREF(ret->maskna_dtype);
        ret->maskna_data = PyArray_MASKNA_DATA(out);
        ret->flags |= NPY_ARRAY_MASKNA;
    }

    return (PyArrayObject *)ret;
}

/*
 * Creates a result for reducing 'operand' along the axes specified
 * in 'axis_flags'. If 'dtype' isn't NULL, this function steals a
 * reference to 'dtype'.
 *
 * If 'out' isn't NULL, this function creates a view conforming
 * to the number of dimensions of 'operand', adding a singleton dimension
 * for each reduction axis specified. In this case, 'dtype' is ignored
 * (but its reference is still stolen), and the caller must handle any
 * type conversion/validity check for 'out'. When 'need_namask' is true,
 * raises an exception if 'out' doesn't have an NA mask.
 *
 * If 'subok' is true, creates a result with the subtype of 'operand',
 * otherwise creates on with the base ndarray class.
 *
 * If 'out' is NULL, it allocates a new array whose shape matches
 * that of 'operand', except for at the reduction axes. An NA mask
 * is added if 'need_namask' is true.  If 'dtype' is NULL, the dtype
 * of 'operand' is used for the result.
 */
NPY_NO_EXPORT PyArrayObject *
PyArray_CreateReduceResult(PyArrayObject *operand, PyArrayObject *out,
                    PyArray_Descr *dtype, npy_bool *axis_flags,
                    int need_namask, int keepdims, int subok,
                    const char *funcname)
{
    PyArrayObject *result;

    if (out == NULL) {
        /* This function steals the reference to 'dtype' */
        result = allocate_reduce_result(operand, axis_flags, dtype, subok);

        /* Allocate an NA mask if necessary */
        if (need_namask && result != NULL) {
            if (PyArray_AllocateMaskNA(result, 1, 0, 1) < 0) {
                Py_DECREF(result);
                return NULL;
            }
        }
    }
    else {
        /* Steal the dtype reference */
        Py_XDECREF(dtype);

        if (need_namask && !PyArray_HASMASKNA(out)) {
            PyErr_Format(PyExc_ValueError,
                    "output parameter for reduction operation %s "
                    "needs an NA mask, but the array provided does "
                    "not have one", funcname);
            return NULL;
        }

        result = conform_reduce_result(PyArray_NDIM(operand), axis_flags,
                                        out, keepdims, funcname);
    }

    return result;
}

/*
 * Checks that there are only zero or one dimensions selected in 'axis_flags',
 * and raises an error about a non-reorderable reduction if not.
 */
static int
check_nonreorderable_axes(int ndim, npy_bool *axis_flags, const char *funcname)
{
    int idim, single_axis = 0;
    for (idim = 0; idim < ndim; ++idim) {
        if (axis_flags[idim]) {
            if (single_axis) {
                PyErr_Format(PyExc_ValueError,
                        "reduction operation '%s' is not reorderable, "
                        "so only one axis may be specified",
                        funcname);
                return -1;
            }
            else {
                single_axis = 1;
            }
        }
    }

    return 0;
}

/*
 * Initializes the reduce result for skipna reductions where operand
 * has more than one dimension.
 *
 * 'operand must have an NA mask, 'result' may or may not have an
 * NA mask, and 'skipna' must be True to call this function.
 */
static PyArrayObject *
initialize_reduce_result_noidentity_skipna(
                    PyArrayObject *operand, PyArrayObject *result,
                    npy_bool *axis_flags, const char *funcname)
{
    PyArrayObject *initialized = NULL;
    npy_intp initialized_countdown;
    npy_intp op_itemsize;
    PyArray_Descr *bool_dtype;

    /* Iterator parameters */
    NpyIter *iter = NULL;
    PyArrayObject *op[3];
    npy_uint32 flags, op_flags[3];
    npy_intp fixed_strides[4];

    /* Data transfer function */
    PyArray_StridedUnaryOp *stransfer = NULL;
    NpyAuxData *transferdata = NULL;
    int needs_api;

    op_itemsize = PyArray_DTYPE(operand)->elsize;

    /*
     * Create a view of operand which owns its its own mask, so that
     * we can change it.
     */
    operand = (PyArrayObject *)PyArray_View(operand, NULL, &PyArray_Type);
    if (operand == NULL) {
        goto fail;
    }
    if (PyArray_AllocateMaskNA(operand, 1, 0, 1) < 0) {
        goto fail;
    }

    /*
     * Allocate a flag array to keep track of which elements in the result
     * have already been initialized.
     *
     * This reference to bool_dtype gets stolen by NewLikeArray.
     */
    bool_dtype = PyArray_DescrFromType(NPY_BOOL);
    if (bool_dtype == NULL) {
        goto fail;
    }
    initialized = (PyArrayObject *)PyArray_NewLikeArray(result,
                                            NPY_KEEPORDER, bool_dtype, 0);
    if (initialized == NULL) {
        goto fail;
    }
    if (PyArray_AssignZero(initialized, NULL, 0, NULL) < 0) {
        Py_DECREF(initialized);
        goto fail;
    }

    /* Set up the iterator for copying the elements */
    op[0] = operand;
    op[1] = result;
    op[2] = initialized;
    op_flags[0] = NPY_ITER_READWRITE | NPY_ITER_USE_MASKNA;
    op_flags[1] = NPY_ITER_READWRITE | NPY_ITER_IGNORE_MASKNA;
    op_flags[2] = NPY_ITER_READWRITE;
    flags = NPY_ITER_EXTERNAL_LOOP |
            NPY_ITER_REFS_OK |
            NPY_ITER_REDUCE_OK |
            NPY_ITER_ZEROSIZE_OK |
            NPY_ITER_DONT_NEGATE_STRIDES;

    iter = NpyIter_MultiNew(3, op, flags,
                               NPY_KEEPORDER, NPY_UNSAFE_CASTING,
                               op_flags,
                               NULL);
    if (iter == NULL) {
        goto fail;
    }
    NpyIter_GetInnerFixedStrideArray(iter, fixed_strides);
    needs_api = NpyIter_IterationNeedsAPI(iter);

    /* Get a function for copying the elements */
    if (PyArray_GetDTypeTransferFunction(
                    PyArray_ISALIGNED(operand) && PyArray_ISALIGNED(result),
                    fixed_strides[0], fixed_strides[1],
                    PyArray_DTYPE(operand), PyArray_DTYPE(result),
                    0,
                    &stransfer, &transferdata,
                    &needs_api) != NPY_SUCCEED) {
        goto fail;
    }

    /*
     * Track how many initializations we've done, both to
     * short circuit completion and to raise an error if
     * any remained uninitialized.
     */
    initialized_countdown = PyArray_SIZE(result);

    if (NpyIter_GetIterSize(iter) != 0) {
        NpyIter_IterNextFunc *iternext;
        char **dataptr;
        npy_intp *strideptr;
        npy_intp *countptr, count, subcount;
        NPY_BEGIN_THREADS_DEF;

        iternext = NpyIter_GetIterNext(iter, NULL);
        if (iternext == NULL) {
            goto fail;
        }
        dataptr = NpyIter_GetDataPtrArray(iter);
        strideptr = NpyIter_GetInnerStrideArray(iter);
        countptr = NpyIter_GetInnerLoopSizePtr(iter);

        if (!needs_api) {
            NPY_BEGIN_THREADS;
        }

        do {
            char *op_d = dataptr[0], *res_d = dataptr[1];
            char *init_d = dataptr[2], *op_namask_d = dataptr[3];
            npy_intp op_s = strideptr[0], res_s = strideptr[1];
            npy_intp init_s = strideptr[2], op_namask_s = strideptr[3];

            count = *countptr;

            /* If the result stride is 0, copy at most one value */
            if (res_s == 0) {
                npy_intp i;
                for (i = 0; i < count; ++i) {
                    if (*init_d == 0 && NpyMaskValue_IsExposed(
                                                *(npy_mask *)op_namask_d)) {

                        /* Mark it as initialized */
                        *init_d = 1;
                        stransfer(res_d, 0, op_d + i * op_s, op_s,
                                        1, op_itemsize, transferdata);

                        --initialized_countdown;
                        if (initialized_countdown == 0) {
                            goto finish_loop;
                        }
                        break;
                    }

                    init_d += init_s;
                    op_namask_d += op_namask_s;
                }
            }
            /* Otherwise process the data in runs as large as possible */
            else {
                do {
                    /* Skip values that are initialized or masked */
                    subcount = 0;
                    while (subcount < count && (*init_d == 1 ||
                                !NpyMaskValue_IsExposed(
                                                *(npy_mask *)op_namask_d))) {
                        ++subcount;
                        init_d += init_s;
                        op_namask_d += op_namask_s;
                    }
                    op_d += subcount * op_s;
                    res_d += subcount * res_s;
                    count -= subcount;

                    /* Transfer values that are uninitialized and exposed */
                    subcount = 0;
                    while (subcount < count && (*init_d == 0 &&
                                NpyMaskValue_IsExposed(
                                                *(npy_mask *)op_namask_d))) {
                        ++subcount;
                        /* Mark it as initialized */
                        *init_d = 1;
                        init_d += init_s;
                        op_namask_d += op_namask_s;
                    }
                    stransfer(res_d, res_s, op_d, op_s,
                                subcount, op_itemsize, transferdata);
                    op_d += subcount * op_s;
                    res_d += subcount * res_s;
                    count -= subcount;

                    initialized_countdown -= subcount;
                    if (initialized_countdown == 0) {
                        goto finish_loop;
                    }
                } while (count > 0);
            }
        } while (iternext(iter));

    finish_loop:
        if (!needs_api) {
            NPY_END_THREADS;
        }
    }

    if (needs_api && PyErr_Occurred()) {
        goto fail;
    }

    /* Since this ufunc has no identity, all elements must be initialized */
    if (initialized_countdown != 0) {
        PyErr_Format(PyExc_ValueError,
                "reduction operation %s with skipna=True "
                "had an output element with all its inputs NA", funcname);
        goto fail;
    }

    /* If 'result' has an NA mask, set it to all exposed */
    if (PyArray_HASMASKNA(result)) {
        if (PyArray_AssignMaskNA(result, 1, NULL, 0, NULL) < 0) {
            goto fail;
        }
    }

    Py_DECREF(initialized);
    NpyIter_Deallocate(iter);
    NPY_AUXDATA_FREE(transferdata);
    return operand;

fail:
    Py_XDECREF(operand);
    Py_XDECREF(initialized);
    if (iter != NULL) {
        NpyIter_Deallocate(iter);
    }
    NPY_AUXDATA_FREE(transferdata);

    return NULL;
}

/*
 * This function initializes a result array for a reduction operation
 * which has no identity. This means it needs to copy the first element
 * it sees along the reduction axes to result, then return a view of
 * the operand which excludes that element.
 *
 * If a reduction has an identity, such as 0 or 1, the result should
 * be initialized by calling PyArray_AssignZero(result, NULL, !skipna, NULL)
 * or PyArray_AssignOne(result, NULL, !skipna, NULL), because this
 * function raises an exception when there are no elements to reduce.
 *
 * For regular reduction, this means it copies the subarray indexed
 * at zero along each reduction axis into 'result', then returns a view
 * into 'operand' excluding those copied elements. If 'operand' has
 * an NA mask in this case, the caller should have already done
 * the reduction on the mask. This function copies the subarray with
 * 'replacena' set to True, so that the already accumulated NA mask
 * in result doesn't get overwritten.
 *
 * For 'skipna' reduction, this is more complicated. In the one dimensional
 * case, it searches for the first non-NA element, copies that element
 * to 'result', then returns a view into the rest of 'operand'. For
 * multi-dimensional reductions, the initial elements may be scattered
 * throughout the array.
 *
 * To deal with this, a view of 'operand' is taken, and given its own
 * copy of the NA mask. Additionally, an array of flags is created,
 * matching the shape of 'result', and initialized to all False.
 * Then, the elements of the 'operand' view are gone through, and any time
 * an exposed element is encounted which isn't already flagged in the
 * auxiliary array, it is copied into 'result' and flagged as copied.
 * The element is masked as an NA in the view of 'operand', so that the
 * later reduction step will skip it during processing.
 *
 * result  : The array into which the result is computed. This must have
 *           the same number of dimensions as 'operand', but for each
 *           axis i where 'axis_flags[i]' is True, it has a single element.
 * operand : The array being reduced.
 * axis_flags : An array of boolean flags, one for each axis of 'operand'.
 *              When a flag is True, it indicates to reduce along that axis.
 * reorderable : If True, the reduction being done is reorderable, which
 *               means specifying multiple axes of reduction at once is ok,
 *               and the reduction code may calculate the reduction in an
 *               arbitrary order. The calculation may be reordered because
 *               of cache behavior or multithreading requirements.
 * skipna  : If True, indicates that the reduction is being calculated
 *           as if the NA values are being dropped from the computation
 *           instead of accumulating into an NA result.
 * out_skip_first_count : This gets populated with the number of first-visit
 *                        elements that should be skipped during the
 *                        iteration loop.
 * funcname : The name of the reduction operation, for the purpose of
 *            better quality error messages. For example, "numpy.max"
 *            would be a good name for NumPy's max function.
 *
 * Returns a view which contains the remaining elements on which to do
 * the reduction.
 */
NPY_NO_EXPORT PyArrayObject *
PyArray_InitializeReduceResult(
                    PyArrayObject *result, PyArrayObject *operand,
                    npy_bool *axis_flags, int reorderable, int skipna,
                    npy_intp *out_skip_first_count, const char *funcname)
{
    npy_intp *strides, *shape, shape_orig[NPY_MAXDIMS], shape0;
    PyArrayObject *op_view = NULL;
    int idim, ndim, nreduce_axes;

    ndim = PyArray_NDIM(operand);

    /* Default to no skipping first-visit elements in the iteration */
    *out_skip_first_count = 0;

    /*
     * If this reduction is non-reorderable, make sure there are
     * only 0 or 1 axes in axis_flags.
     */
    if (!reorderable && check_nonreorderable_axes(ndim,
                                    axis_flags, funcname) < 0) {
        return NULL;
    }

    /*
     * If 'skipna' is False, or 'operand' has no NA mask in which
     * case the 'skipna' flag does nothing.
     */
    if (!skipna || !PyArray_HASMASKNA(operand)) {
        if (PyArray_SIZE(operand) == 0) {
            PyErr_Format(PyExc_ValueError,
                    "zero-size array to reduction operation %s "
                    "which has no identity",
                    funcname);
            return NULL;
        }

        /* Take a view into 'operand' which we can modify. */
        op_view = (PyArrayObject *)PyArray_View(operand, NULL, &PyArray_Type);
        if (op_view == NULL) {
            return NULL;
        }
    }
    /*
     * Otherwise 'skipna' is True and 'operand' has an NA mask. Deal
     * with the simple one-dimensional case first
     */
    else if (ndim == 1) {
        char *data, *maskna_data;
        npy_intp *maskna_strides;

        ndim = PyArray_NDIM(operand);

        op_view = (PyArrayObject *)PyArray_View(operand, NULL, &PyArray_Type);
        if (op_view == NULL) {
            return NULL;
        }

        shape = PyArray_DIMS(op_view);
        shape0 = shape[0];
        data = PyArray_DATA(op_view);
        strides = PyArray_STRIDES(op_view);
        maskna_data = PyArray_MASKNA_DATA(op_view);
        maskna_strides = PyArray_MASKNA_STRIDES(op_view);

        /* Shrink the array from the start until we find an exposed element */
        while (shape0 > 0 &&
                    !NpyMaskValue_IsExposed((npy_mask)*maskna_data)) {
            --shape0;
            data += strides[0];
            maskna_data += maskna_strides[0];
        }

        if (shape0 == 0) {
            Py_DECREF(op_view);
            PyErr_Format(PyExc_ValueError,
                    "fully NA array with skipna=True to reduction operation "
                    "%s which has no identity", funcname);
            return NULL;
        }

        /*
         * With the first element exposed, fall through to the code
         * which copies the element and adjusts the view just as in the
         * non-skipna case.
         */
        shape[0] = shape0;
        ((PyArrayObject_fields *)op_view)->data = data;
        ((PyArrayObject_fields *)op_view)->maskna_data = maskna_data;
    }
    /*
     * Here 'skipna' is True and 'operand' has an NA mask, but
     * 'operand' has more than one dimension, so it's the complicated
     * case
     */
    else {
        return initialize_reduce_result_noidentity_skipna(
                                    operand, result, axis_flags, funcname);
    }

    /*
     * Now copy the subarray of the first element along each reduction axis,
     * then return a view to the rest.
     *
     * Adjust the shape to only look at the first element along
     * any of the reduction axes. We count the number of reduction axes
     * at the same time.
     */
    shape = PyArray_SHAPE(op_view);
    nreduce_axes = 0;
    memcpy(shape_orig, shape, ndim * sizeof(npy_intp));
    for (idim = 0; idim < ndim; ++idim) {
        if (axis_flags[idim]) {
            shape[idim] = 1;
            ++nreduce_axes;
        }
    }

    /*
     * Copy the elements into the result to start, with
     * 'preservena' set to True so that we don't overwrite
     * what we already calculated in ReduceNAMask.
     */
    if (PyArray_AssignArray(result, op_view, NULL, NPY_UNSAFE_CASTING,
                                                            1, NULL) < 0) {
        Py_DECREF(op_view);
        return NULL;
    }

    /*
     * If there is one reduction axis, adjust the view's
     * shape to only look at the remaining elements
     */
    if (nreduce_axes == 1) {
        strides = PyArray_STRIDES(op_view);
        for (idim = 0; idim < ndim; ++idim) {
            if (axis_flags[idim]) {
                shape[idim] = shape_orig[idim] - 1;
                ((PyArrayObject_fields *)op_view)->data += strides[idim];
            }
        }
        if (PyArray_HASMASKNA(op_view)) {
            strides = PyArray_MASKNA_STRIDES(op_view);
            for (idim = 0; idim < ndim; ++idim) {
                if (axis_flags[idim]) {
                    ((PyArrayObject_fields *)op_view)->maskna_data +=
                                                                strides[idim];
                }
            }
        }
    }
    /* If there are zero reduction axes, make the view empty */
    else if (nreduce_axes == 0) {
        for (idim = 0; idim < ndim; ++idim) {
            shape[idim] = 0;
        }
    }
    /*
     * Otherwise iterate over the whole operand, but tell the inner loop
     * to skip the elements we already copied by setting the skip_first_count.
     */
    else {
        *out_skip_first_count = PyArray_SIZE(result);

        Py_DECREF(op_view);
        Py_INCREF(operand);
        op_view = operand;
    }

    return op_view;
}

/*NUMPY_API
 *
 * This function executes all the standard NumPy reduction function
 * boilerplate code, just calling assign_identity and the appropriate
 * inner loop function where necessary.
 *
 * operand     : The array to be reduced.
 * out         : NULL, or the array into which to place the result.
 * wheremask   : NOT YET SUPPORTED, but this parameter is placed here
 *               so that support can be added in the future without breaking
 *               API compatibility. Pass in NULL.
 * operand_dtype : The dtype the inner loop expects for the operand.
 * result_dtype : The dtype the inner loop expects for the result.
 * casting     : The casting rule to apply to the operands.
 * axis_flags  : Flags indicating the reduction axes of 'operand'.
 * reorderable : If True, the reduction being done is reorderable, which
 *               means specifying multiple axes of reduction at once is ok,
 *               and the reduction code may calculate the reduction in an
 *               arbitrary order. The calculation may be reordered because
 *               of cache behavior or multithreading requirements.
 * skipna      : If true, NAs are skipped instead of propagating.
 * whichskipna : NOT YET SUPPORTED, but this parameter is placed here
 *               so that support can be added for multi-NA without
 *               breaking API compatibility. Pass in NULL.
 * keepdims    : If true, leaves the reduction dimensions in the result
 *               with size one.
 * subok       : If true, the result uses the subclass of operand, otherwise
 *               it is always a base class ndarray.
 * assign_identity : If NULL, PyArray_InitializeReduceResult is used, otherwise
 *               this function is called to initialize the result to
 *               the reduction's unit.
 * loop        : The loop which does the reduction.
 * masked_loop : The loop which does the reduction with a mask.
 * advanced_masked_loop: If non-NULL, this is a loop which uses a mask from
 *               both the operand and the result. The 'result' is
 *               initialized to a usual reduction of the operand's mask,
 *               but both the operand's mask and the result's mask
 *               are provided so that the loop may decide to expose
 *               elements, which normally would not be exposed by the
 *               normal NA propagation rules, based on the input data.
 * data        : Data which is passed to assign_identity and the inner loop.
 * buffersize  : Buffer size for the iterator. For the default, pass in 0.
 * funcname    : The name of the reduction function, for error messages.
 */
NPY_NO_EXPORT PyArrayObject *
PyArray_ReduceWrapper(PyArrayObject *operand, PyArrayObject *out,
                        PyArrayObject *wheremask,
                        PyArray_Descr *operand_dtype,
                        PyArray_Descr *result_dtype,
                        NPY_CASTING casting,
                        npy_bool *axis_flags, int reorderable,
                        int skipna, npy_bool *skipwhichna, int keepdims,
                        int subok,
                        PyArray_AssignReduceIdentityFunc *assign_identity,
                        PyArray_ReduceLoopFunc *loop,
                        PyArray_ReduceLoopFunc *masked_loop,
                        PyArray_ReduceLoopFunc *advanced_masked_loop,
                        void *data, npy_intp buffersize, const char *funcname)
{
    int use_maskna;
    PyArrayObject *result = NULL, *op_view = NULL;
    npy_intp skip_first_count = 0;

    /* Iterator parameters */
    NpyIter *iter = NULL;
    PyArrayObject *op[2];
    PyArray_Descr *op_dtypes[2];
    npy_uint32 flags, op_flags[2];

    /* Validate that the parameters for future expansion are NULL */
    if (wheremask != NULL) {
        PyErr_SetString(PyExc_RuntimeError,
                "Reduce operations in NumPy do not yet support "
                "a where mask");
        return NULL;
    }
    if (skipwhichna != NULL) {
        PyErr_SetString(PyExc_RuntimeError,
                "multi-NA support is not yet implemented in "
                "reduce operations");
        return NULL;
    }

    use_maskna = PyArray_HASMASKNA(operand);

    /*
     * If 'operand' has an NA mask, but 'out' doesn't, validate that 'operand'
     * contains no NA values so we can ignore the mask entirely.
     */
    if (use_maskna && !skipna && out != NULL && !PyArray_HASMASKNA(out)) {
        int containsna = PyArray_ContainsNA(operand, wheremask, NULL);
        if (containsna == -1) {
            goto fail;
        }
        else if (containsna) {
            PyErr_SetString(PyExc_ValueError,
                    "Cannot assign NA to an array which "
                    "does not support NAs");
            goto fail;
        }
        else {
            use_maskna = 0;
        }
    }

    /*
     * This either conforms 'out' to the ndim of 'operand', or allocates
     * a new array appropriate for this reduction.
     */
    Py_INCREF(result_dtype);
    result = PyArray_CreateReduceResult(operand, out,
                            result_dtype, axis_flags, !skipna && use_maskna,
                            keepdims, subok, funcname);
    if (result == NULL) {
        goto fail;
    }

    /*
     * Do the reduction on the NA mask before the data. This way
     * we can avoid modifying the outputs which end up masked, obeying
     * the required NA masking semantics.
     */
    if (use_maskna && !skipna) {
        if (PyArray_ReduceMaskNAArray(operand, result, NULL, NULL) < 0) {
            goto fail;
        }

        /*
         * Short circuit any calculation if the result is a 0-dim NA
         * and the advanced masked loop which could expose it isn't
         * provided.
         */
        if (advanced_masked_loop == NULL && PyArray_SIZE(result) == 1 &&
                !NpyMaskValue_IsExposed(
                            (npy_mask)*PyArray_MASKNA_DATA(result))) {
            goto finish;
        }
    }

    /*
     * Initialize the result to the reduction unit if possible,
     * otherwise copy the initial values and get a view to the rest.
     */
    if (assign_identity != NULL) {
        /*
         * If this reduction is non-reorderable, make sure there are
         * only 0 or 1 axes in axis_flags.
         */
        if (!reorderable && check_nonreorderable_axes(PyArray_NDIM(operand),
                                        axis_flags, funcname) < 0) {
            goto fail;
        }

        if (assign_identity(result, !skipna, data) < 0) {
            goto fail;
        }
        op_view = operand;
        Py_INCREF(op_view);
    }
    else {
        op_view = PyArray_InitializeReduceResult(result, operand,
                            axis_flags, reorderable, skipna,
                            &skip_first_count, funcname);
        if (op_view == NULL) {
            goto fail;
        }
        if (PyArray_SIZE(op_view) == 0) {
            Py_DECREF(op_view);
            op_view = NULL;
            goto finish;
        }
    }

    /* Set up the iterator */
    op[0] = result;
    op[1] = op_view;
    op_dtypes[0] = result_dtype;
    op_dtypes[1] = operand_dtype;

    flags = NPY_ITER_BUFFERED |
            NPY_ITER_EXTERNAL_LOOP |
            NPY_ITER_GROWINNER |
            NPY_ITER_DONT_NEGATE_STRIDES |
            NPY_ITER_ZEROSIZE_OK |
            NPY_ITER_REDUCE_OK |
            NPY_ITER_REFS_OK;
    op_flags[0] = NPY_ITER_READWRITE |
                  NPY_ITER_ALIGNED |
                  NPY_ITER_NO_SUBTYPE;
    op_flags[1] = NPY_ITER_READONLY |
                  NPY_ITER_ALIGNED;

    /* Add mask-related flags */
    if (use_maskna) {
        if (skipna) {
            /* The output's mask has been set to all exposed already */
            op_flags[0] |= NPY_ITER_IGNORE_MASKNA;
            /* Need the input's mask to determine what to skip */
            op_flags[1] |= NPY_ITER_USE_MASKNA;
        }
        else {
            /* Iterate over the output's mask */
            op_flags[0] |= NPY_ITER_USE_MASKNA;
            if (advanced_masked_loop == NULL) {
                /* Input's mask is already incorporated in the output's mask */
                op_flags[1] |= NPY_ITER_IGNORE_MASKNA;
            }
            else {
                /* The reduction wants to use the operand's mask as well */
                op_flags[1] |= NPY_ITER_USE_MASKNA;
            }
        }
    }
    else {
        /*
         * If 'out' had no mask, and 'operand' did, we checked that 'operand'
         * contains no NA values and can ignore the masks.
         */
        op_flags[0] |= NPY_ITER_IGNORE_MASKNA;
        op_flags[1] |= NPY_ITER_IGNORE_MASKNA;
    }

    iter = NpyIter_AdvancedNew(2, op, flags,
                               NPY_KEEPORDER, casting,
                               op_flags,
                               op_dtypes,
                               0, NULL, NULL, buffersize);
    if (iter == NULL) {
        goto fail;
    }

    if (NpyIter_GetIterSize(iter) != 0) {
        NpyIter_IterNextFunc *iternext;
        char **dataptr;
        npy_intp *strideptr;
        npy_intp *countptr;
        int needs_api;

        iternext = NpyIter_GetIterNext(iter, NULL);
        if (iternext == NULL) {
            goto fail;
        }
        dataptr = NpyIter_GetDataPtrArray(iter);
        strideptr = NpyIter_GetInnerStrideArray(iter);
        countptr = NpyIter_GetInnerLoopSizePtr(iter);

        needs_api = NpyIter_IterationNeedsAPI(iter);

        /* Straightforward reduction */
        if (!use_maskna) {
            if (loop == NULL) {
                PyErr_Format(PyExc_RuntimeError,
                        "reduction operation %s did not supply an "
                        "unmasked inner loop function", funcname);
                goto fail;
            }

            if (loop(iter, dataptr, strideptr, countptr,
                            iternext, needs_api, skip_first_count, data) < 0) {

                goto fail;
            }
        }
        /* Masked reduction with both masks */
        else if (!skipna && advanced_masked_loop != NULL) {
            if (advanced_masked_loop(iter, dataptr, strideptr, countptr,
                            iternext, needs_api, skip_first_count, data) < 0) {
                goto fail;
            }
        }
        /* Regular masked reduction with just one mask */
        else {
            if (masked_loop == NULL) {
                PyErr_Format(PyExc_RuntimeError,
                        "reduction operation %s did not supply a "
                        "masked inner loop function", funcname);
                goto fail;
            }

            if (masked_loop(iter, dataptr, strideptr, countptr,
                            iternext, needs_api, skip_first_count, data) < 0) {
                goto fail;
            }
        }
    }

    NpyIter_Deallocate(iter);
    Py_DECREF(op_view);

finish:
    /* Strip out the extra 'one' dimensions in the result */
    if (out == NULL) {
        if (!keepdims) {
            PyArray_RemoveAxesInPlace(result, axis_flags);
        }
    }
    else {
        Py_DECREF(result);
        result = out;
        Py_INCREF(result);
    }

    return result;

fail:
    Py_XDECREF(result);
    Py_XDECREF(op_view);
    if (iter != NULL) {
        NpyIter_Deallocate(iter);
    }

    return NULL;
}

/*
 * This function counts the number of elements that a reduction
 * will see along the reduction directions, given the provided options.
 *
 * If the reduction operand has no NA mask or 'skipna' is false, this
 * is simply the prod`uct of all the reduction axis sizes. A NumPy
 * scalar is returned in this case.
 *
 * If the reduction operand has an NA mask and 'skipna' is true, this
 * counts the number of elements which are not NA along the reduction
 * dimensions, and returns an array with the counts.
 */
NPY_NO_EXPORT PyObject *
PyArray_CountReduceItems(PyArrayObject *operand,
                            npy_bool *axis_flags, int skipna, int keepdims)
{
    int idim, ndim = PyArray_NDIM(operand);

    /* The product of the reduction dimensions in this case */
    if (!skipna || !PyArray_HASMASKNA(operand)) {
        npy_intp count = 1, *shape = PyArray_SHAPE(operand);
        PyArray_Descr *dtype;
        PyObject *ret;

        for (idim = 0; idim < ndim; ++idim) {
            if (axis_flags[idim]) {
                count *= shape[idim];
            }
        }

        dtype = PyArray_DescrFromType(NPY_INTP);
        if (dtype == NULL) {
            return NULL;
        }
        ret = PyArray_Scalar(&count, dtype, NULL);
        Py_DECREF(dtype);
        return ret;
    }
    /* Otherwise we need to do a count based on the NA mask */
    else {
        npy_intp *strides;
        PyArrayObject *result;
        PyArray_Descr *result_dtype;

        npy_intp i, coord[NPY_MAXDIMS];
        npy_intp shape_it[NPY_MAXDIMS];
        npy_intp operand_strides_it[NPY_MAXDIMS];
        npy_intp result_strides_it[NPY_MAXDIMS];
        char *operand_data = NULL, *result_data = NULL;

        /*
         * To support field-NA, we would create a result type
         * with an INTP matching each field, then separately count
         * the available elements per-field.
         */
        if (PyArray_HASFIELDS(operand)) {
            PyErr_SetString(PyExc_RuntimeError,
                    "field-NA isn't implemented yet");
            return NULL;
        }

        /*
         * TODO: The loop below is specialized for NPY_BOOL masks,
         *       will need another version for NPY_MASK masks.
         */
        if (PyArray_MASKNA_DTYPE(operand)->type_num != NPY_BOOL) {
            PyErr_SetString(PyExc_RuntimeError,
                    "multi-NA isn't implemented yet");
            return NULL;
        }

        /* Allocate an array for the reduction counting */
        result_dtype = PyArray_DescrFromType(NPY_INTP);
        if (result_dtype == NULL) {
            return NULL;
        }
        result = PyArray_CreateReduceResult(operand, NULL,
                                result_dtype, axis_flags, 0,
                                keepdims, 0, "count_reduce_items");
        if (result == NULL) {
            return NULL;
        }

        /* Initialize result to all zeros */
        if (PyArray_AssignZero(result, NULL, 0, NULL) < 0) {
            Py_DECREF(result);
            return NULL;
        }

        /*
         * Set all the reduction strides to 0 in result so
         * we can use them for raw iteration
         */
        strides = PyArray_STRIDES(result);
        for (idim = 0; idim < ndim; ++idim) {
            if (axis_flags[idim]) {
                strides[idim] = 0;
            }
        }

        /*
         * Sort axes based on 'operand', which has more non-zero strides,
         * by making it the first operand here
         */
        if (PyArray_PrepareTwoRawArrayIter(ndim, PyArray_SHAPE(operand),
                PyArray_MASKNA_DATA(operand), PyArray_MASKNA_STRIDES(operand),
                            PyArray_DATA(result), PyArray_STRIDES(result),
                            &ndim, shape_it,
                            &operand_data, operand_strides_it,
                            &result_data, result_strides_it) < 0) {
            Py_DECREF(result);
            return NULL;
        }

        /*
         * NOTE: The following only works for NPY_BOOL masks.
         */
        NPY_RAW_ITER_START(idim, ndim, coord, shape_it) {
            char *operand_d = operand_data, *result_d = result_data;
            for (i = 0; i < shape_it[0]; ++i) {
                *(npy_intp *)result_d += *operand_d;

                operand_d += operand_strides_it[0];
                result_d += result_strides_it[0];
            }
        } NPY_RAW_ITER_TWO_NEXT(idim, ndim, coord, shape_it,
                                    operand_data, operand_strides_it,
                                    result_data, result_strides_it);

        /* Remove the reduction axes and return the result */
        if (!keepdims) {
            PyArray_RemoveAxesInPlace(result, axis_flags);
        }
        return PyArray_Return(result);
    }
}
