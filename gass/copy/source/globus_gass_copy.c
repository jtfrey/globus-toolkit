/**
 * @file globus_gass_copy.c
 *
 * Globus GASS Copy library
 *
 * @see See the detailed description in globus_gass_copy.h
 */

#include "globus_gass_copy.h"
#include <sys/timeb.h>
#include "version.h"

#ifndef GLOBUS_DONT_DOCUMENT_INTERNAL

static int globus_l_gass_copy_activate(void);

static int globus_l_gass_copy_deactivate(void);

struct globus_gass_copy_perf_info_s
{
    globus_gass_copy_handle_t *             copy_handle;

    globus_gass_copy_performance_cb_t       callback;
    void *                                  user_arg;

    globus_ftp_client_plugin_t              ftp_perf_plugin;

    /* for 3pt only (may need to set EB mode) */
    globus_bool_t                           saved_dest_attr;
    globus_bool_t                           saved_source_attr;
    globus_ftp_client_operationattr_t *     dest_ftp_attr;
    globus_ftp_client_operationattr_t *     source_ftp_attr;

    /* for local callback computation only */
    globus_callback_handle_t                local_cb_handle;

    globus_mutex_t                          lock;

    double                                  start_time;

    double                                  prev_time;
    globus_off_t                            prev_bytes;

    globus_off_t                            live_bytes;
};

static
void
globus_l_gass_copy_perf_ftp_cb(
    void *                                  user_arg,
    globus_ftp_client_handle_t *            handle,
    globus_off_t                            bytes,
    float                                   instantaneous_throughput,
    float                                   avg_throughput);

static
void
globus_l_gass_copy_perf_setup_local_callback(
    globus_gass_copy_perf_info_t *          perf_info);

static
void
globus_l_gass_copy_perf_setup_ftp_callback(
    globus_gass_copy_perf_info_t *          perf_info);

static
void
globus_l_gass_copy_perf_cancel_local_callback(
    globus_gass_copy_perf_info_t *          perf_info);

static
void
globus_l_gass_copy_perf_cancel_ftp_callback(
    globus_gass_copy_perf_info_t *          perf_info);

/* uncomment this line for debug messages */
/* #define GLOBUS_I_GASS_COPY_DEBUG */


#define globus_i_gass_copy_set_error(handle, error) \
{ \
    if(handle->err == GLOBUS_NULL) \
        handle->err = globus_object_copy(error); \
}

#define globus_i_gass_copy_set_error_from_result(handle, result) \
{ \
    if(handle->err == GLOBUS_NULL) \
    { \
        globus_object_t *tmp_err; \
        tmp_err = globus_error_get(result); \
        handle->err = globus_object_copy(tmp_err); \
	result = globus_error_put(tmp_err); \
    } \
}

#include "globus_i_gass_copy.h"
#include "globus_ftp_client_throughput_plugin.h"
#include <time.h>

/******************************************************************************
                       Define module specific variables
******************************************************************************/

globus_module_descriptor_t globus_i_gass_copy_module =
{
    "globus_gass_copy",
    globus_l_gass_copy_activate,
    globus_l_gass_copy_deactivate,
    GLOBUS_NULL,
    GLOBUS_NULL,
    &local_version
};

/*
****************************************
  module activation
******************************************/
static
int
globus_l_gass_copy_activate(void)
{
    int rc;

    rc = globus_module_activate(GLOBUS_GASS_TRANSFER_MODULE);
    if (rc != GLOBUS_SUCCESS)
    {
	return(rc);
    }

    rc = globus_module_activate(GLOBUS_IO_MODULE);
    if (rc != GLOBUS_SUCCESS)
    {
	return(rc);
    }
    rc = globus_module_activate(GLOBUS_FTP_CLIENT_MODULE);
    if (rc != GLOBUS_SUCCESS)
    {
	return(rc);
    }

    rc = globus_module_activate(GLOBUS_FTP_CLIENT_THROUGHPUT_PLUGIN_MODULE);
    if (rc != GLOBUS_SUCCESS)
    {
	return(rc);
    }

    return 0;
} /* globus_l_gass_copy_activate() */

/*****************************************
  module deactivation
******************************************/
static
int
globus_l_gass_copy_deactivate(void)
{
    int rc;
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr,"GASS_COPY: about to globus_module_deactivate(GLOBUS_FTP_CLIENT_MODULE) \n");
#endif
    rc = globus_module_deactivate(GLOBUS_FTP_CLIENT_MODULE);

    if (rc != GLOBUS_SUCCESS)
    {
	return(rc);
    }

#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr,"GASS_COPY: about to globus_module_deactivate(GLOBUS_IO_MODULE) \n");
#endif

    rc = globus_module_deactivate(GLOBUS_IO_MODULE);
    if (rc != GLOBUS_SUCCESS)
    {
	return(rc);
    }

#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr,"GASS_COPY: about to globus_module_deactivate(GLOBUS_GASS_TRANSFER_MODULE) \n");
#endif

    rc = globus_module_deactivate(GLOBUS_GASS_TRANSFER_MODULE);
    if (rc != GLOBUS_SUCCESS)
    {
	return(rc);
    }
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr,"GASS_COPY: done globus_module_deactivate(GLOBUS_GASS_TRANSFER_MODULE) \n");
#endif

    rc = globus_module_deactivate(GLOBUS_FTP_CLIENT_THROUGHPUT_PLUGIN_MODULE);
    if (rc != GLOBUS_SUCCESS)
    {
	return(rc);
    }
    return 0;
} /* globus_i_gass_copy_deactivate() */


/********************************************************************
 * generic callback to signal completion of asynchronous transfer
 ********************************************************************/
static
void
globus_l_gass_copy_monitor_callback(
    void * callback_arg,
    globus_gass_copy_handle_t * handle,
    globus_object_t * error)
{
    globus_i_gass_copy_monitor_t       *monitor;
    monitor = (globus_i_gass_copy_monitor_t*)callback_arg;

    globus_mutex_lock(&monitor->mutex);
    monitor->done = GLOBUS_TRUE;
    if(error != GLOBUS_NULL)
    {
	monitor->err = globus_object_copy(error);
	monitor->use_err = GLOBUS_TRUE;
    }
    globus_cond_signal(&monitor->cond);
    globus_mutex_unlock(&monitor->mutex);

    return;
} /* globus_l_gass_copy_monitor_callback() */

#endif

/************************************************************
 * Handle initialization and destruction
 ************************************************************/

/**
 * Initialize a GASS Copy handle
 *
 * A globus_gass_copy_handle must be initialized before any transfers may be
 * associated with it.  This function initializes a globus_gass_copy_handle
 * to be used for doing transfers, this includes initializing a
 * globus_ftp_client_handle which will be used for doing any ftp/gsiftp
 * transfers. The same handle may be used to perform multiple, consecutive
 * transfers.  However, there can only be one transfer associated with a
 * particular handle at any given time.  After all transfers to be associated
 * with this handle have completed, the handle should be destroyed by calling
 * globus_gass_copy_handle_destroy().
 *
 * @param handle
 *       The handle to be initialized
 * @param attr
 *       The handle attributes used to use with this handle
 * @return
 *       This function returns GLOBUS_SUCCESS if successful, or a
 *       globus_result_t indicating the error that occurred.
 *
 * @see globus_gass_copy_handle_destroy() ,
 *       globus_gass_copy_handleattr_init(),
 *       globus_ftp_client_hande_init()
 */
globus_result_t
globus_gass_copy_handle_init(
    globus_gass_copy_handle_t * handle,
    globus_gass_copy_handleattr_t * attr)
{
    globus_result_t result;
    globus_object_t * err;
    static char * myname="globus_gass_copy_handle_init";

#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr, "copy_handle_init() was called.....\n");
#endif

    if(handle != GLOBUS_NULL)
    {
	globus_ftp_client_handleattr_t * ftp_attr;

	ftp_attr = (attr && attr->ftp_attr) ? attr->ftp_attr : GLOBUS_NULL;

        result = globus_ftp_client_handle_init(&handle->ftp_source_handle,
                                               ftp_attr);
        if (result != GLOBUS_SUCCESS)
            return result;

        result = globus_ftp_client_handle_init(&handle->ftp_dest_handle,
                                               ftp_attr);
        if (result != GLOBUS_SUCCESS)
            return result;

        handle->external_third_party = GLOBUS_FALSE;
	handle->no_third_party_transfers = GLOBUS_FALSE;
	handle->state = GLOBUS_NULL;
	handle->performance = GLOBUS_NULL;
	handle->status = GLOBUS_GASS_COPY_STATUS_NONE;
	handle->buffer_length = 1024*1024;
	handle->user_pointer = GLOBUS_NULL;
	handle->err = GLOBUS_NULL;

	return GLOBUS_SUCCESS;
    }
    else
    {
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: BAD_PARAMETER, handle is NULL",
	    myname);

	return globus_error_put(err);
    }
}

/**
 *  Destroy a GASS Copy handle
 *
 * Destroy a  gass_copy_handle, which was initialized using
 * globus_gass_copy_handle_init(), that will no longer be used for doing
 * transfers.  Once the handle is detroyed, no further transfers should be
 * associated with it.
 *
 * @param handle
 *       The handle to be destroyed
 *
 * @return
 *       This function returns GLOBUS_SUCCESS if successful, or a
 *       globus_result_t indicating the error that occurred.
 *
 * @see globus_gass_copy_handle_init(),
 *       globus_ftp_client_handle_destroy()
 */
globus_result_t
globus_gass_copy_handle_destroy(
    globus_gass_copy_handle_t * handle)
{
    globus_result_t result;
    globus_object_t * err;
    static char * myname="globus_gass_copy_handle_destroy";

    if(handle != GLOBUS_NULL)
    {
        result = globus_ftp_client_handle_destroy(&handle->ftp_source_handle);
        if (result != GLOBUS_SUCCESS)
            return result;

        result = globus_ftp_client_handle_destroy(&handle->ftp_dest_handle);

	if(handle->err != GLOBUS_NULL)
             globus_object_free(handle->err);

        handle->err = GLOBUS_NULL;

        if(handle->performance)
        {
            globus_ftp_client_throughput_plugin_destroy(
                &handle->performance->ftp_perf_plugin);
            globus_mutex_destroy(&handle->performance->lock);
            globus_free(handle->performance);
            handle->performance = GLOBUS_NULL;
        }

        return result;
    }
    else
    {
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: BAD_PARAMETER, handle is NULL",
	    myname);

	return globus_error_put(err);
    }
}

globus_result_t
globus_gass_copy_handleattr_init(
    globus_gass_copy_handleattr_t * handle_attr)
{
    globus_object_t * err;
    static char * myname = "globus_gass_copy_handleattr_init";

    if(handle_attr)
    {
	handle_attr->ftp_attr = GLOBUS_NULL;

	return GLOBUS_SUCCESS;
    }
    else
    {
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: BAD_PARAMETER, handle_attr is NULL",
	    myname);

	return globus_error_put(err);
    }
}

globus_result_t
globus_gass_copy_handleattr_destroy(
    globus_gass_copy_handleattr_t * handle_attr)
{
    globus_object_t * err;
    static char * myname="globus_gass_copy_handleattr_destroy";

    if(handle_attr != GLOBUS_NULL)
    {
	handle_attr->ftp_attr = GLOBUS_NULL;

        return GLOBUS_SUCCESS;
    }
    else
    {
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: BAD_PARAMETER, handle_attr is NULL",
	    myname);

	return globus_error_put(err);
    }
}

globus_result_t
globus_gass_copy_handleattr_set_ftp_attr(
    globus_gass_copy_handleattr_t * handle_attr,
    globus_ftp_client_handleattr_t * ftp_attr)
{
    globus_object_t * err;
    static char * myname="globus_gass_copy_handleattr_set_ftp_attr";

    if(handle_attr != GLOBUS_NULL)
    {
	handle_attr->ftp_attr = ftp_attr;

        return GLOBUS_SUCCESS;
    }
    else
    {
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: BAD_PARAMETER, handle_attr is NULL",
	    myname);

	return globus_error_put(err);
    }
}


/**
 * Set the size of the buffer to be used for doing transfers
 *
 * This function allows the user to set the size of the buffer that will be
 * used for doing transfers, if this function is not called the buffer size
 * will default to 1M.
 *
 * @param handle
 *       Set the buffer length for transfers associated with this handle.
 * @param length
 *       The length, in bytes, to make the buffer.
 *
 * @return
 *       This function returns GLOBUS_SUCCESS if successful, or a
 *       globus_result_t indicating the error that occurred.
 */
globus_result_t
globus_gass_copy_set_buffer_length(
    globus_gass_copy_handle_t * handle,
    int length)
{
    globus_object_t * err;
    static char * myname="globus_gass_copy_set_buffer_length";
    if (handle)
    {
	handle->buffer_length = length;
	return GLOBUS_SUCCESS;
    }
    else
    {
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: BAD_PARAMETER, handle is NULL",
	    myname);

	return globus_error_put(err);
    }
} /* globus_gass_copy_set_buffer_length() */

/**
 * Get the size of the buffer being used for doing transfers
 *
 * This function allows the user to get the size of the buffer that is being
 * used for doing transfers.
 *
 * @param handle
 *       Get the buffer length for transfers associated with this handle.
 * @param length
 *       The length, in bytes, of the buffer.
 *
 * @return
 *       This function returns GLOBUS_SUCCESS if successful, or a
 *       globus_result_t indicating the error that occurred.
 */
globus_result_t
globus_gass_copy_get_buffer_length(
    globus_gass_copy_handle_t * handle,
    int * length)
{
    globus_object_t * err;
    static char * myname="globus_gass_copy_get_buffer_length";
    if (handle)
    {
	*length = handle->buffer_length;
	return GLOBUS_SUCCESS;
    }
    else
    {
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: BAD_PARAMETER, handle is NULL",
	    myname);

	return globus_error_put(err);
    }
} /* globus_gass_copy_get_buffer_length() */

/**
 * Turn third-party transfers on or off. (They are on by default.)
 *
 * This function allows the user to turn third-party transfers on or off for
 * ftp to ftp transfers associated with a particular handle.  This is often desired
 * if one of the servers involved in the transfer does not allow third-party transfers.
 *
 * @param handle
 *       Turn third-party transfers on or off for transfers associated with this handle.
 *       They are on by default.
 * @param no_third_party_transfers
 *       GLOBUS_FALSE if third-party transfers should be used.
 *       GLOBUS_TRUE if third-party transfers should not be used.
 *
 * @return
 *       This function returns GLOBUS_SUCCESS if successful, or a
 *       globus_result_t indicating the error that occurred.
 */
globus_result_t
globus_gass_copy_set_no_third_party_transfers(
    globus_gass_copy_handle_t * handle,
    globus_bool_t no_third_party_transfers)
{
    globus_object_t * err;
    static char * myname="globus_gass_copy_set_no_third_party_transfers";
    if (handle)
    {
      globus_gass_copy_status_t current_status;
      globus_gass_copy_get_status(handle, &current_status);

      if(current_status == GLOBUS_GASS_COPY_STATUS_PENDING)
      {
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: Cannot change the value of no_third_party_transfers,"
                   "\tthere is a transfer currently pending on this handle",
	    myname);
	return globus_error_put(err);
      }
      else
	handle->no_third_party_transfers = no_third_party_transfers;
	return GLOBUS_SUCCESS;
    }
    else
    {
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: BAD_PARAMETER, handle is NULL",
	    myname);

	return globus_error_put(err);
    }
} /* globus_gass_copy_set_no_third_party_transfers() */

/**
 * See if third-party transfers are turned on or off. (They are on by default.)
 *
 * This function allows the user to see if third-party transfers are turned on or off for
 * ftp to ftp transfers associated with a particular handle.  This is often desired
 * if one of the servers involved in the transfer does not allow third-party transfers.
 *
 * @param handle
 *       See if third-party transfers are turned on or off for transfers associated with this handle.
 *       They are on by default.
 * @param no_third_party_transfers
 *       GLOBUS_FALSE if third-party transfers should be used.
 *       GLOBUS_TRUE if third-party transfers should not be used.
 *
 * @return
 *       This function returns GLOBUS_SUCCESS if successful, or a
 *       globus_result_t indicating the error that occurred.
 */
globus_result_t
globus_gass_copy_get_no_third_party_transfers(
    globus_gass_copy_handle_t * handle,
    globus_bool_t * no_third_party_transfers)
{
    globus_object_t * err;
    static char * myname="globus_gass_copy_get_no_third_party_transfers";
    if (handle)
    {
	*no_third_party_transfers = handle->no_third_party_transfers;
	return GLOBUS_SUCCESS;
    }
    else
    {
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: BAD_PARAMETER, handle is NULL",
	    myname);

	return globus_error_put(err);
    }
} /* globus_gass_copy_get_no_third_party_transfers() */

/**
 * Initialize an attribute structure
 *
 * The globus_gass_copy_attr_t can be used to pass the globus_gass_copy library
 * information about how a transfer should be performed.
 * It must first be initialized by calling this function. Then any or all of
 * the following functions may be called to set attributes associated with a
 * particular protocol: globus_gass_copy_attr_set_ftp(),
 *                      globus_gass_copy_attr_set_gass(),
 *                      globus_gass_copy_attr_set_io().
 * Any function which takes a globus_gass_copy_attr_t as an argument will also
 * accept GLOBUS_NULL, in which case the appropriate set of default attributes
 * will be used.
 *
 * @param attr
 *      The attribute structure to be initialized
 *
 * @return
 *      This function returns GLOBUS_SUCCESS if successful, or a
 *      globus_result_t indicating the error that occurred.
 *
 * @see globus_gass_copy_attr_set_ftp(),
 *      globus_gass_copy_attr_set_gass(),
 *      globus_gass_copy_attr_set_io(),
 *      globus_gass_copy_get_url_mode().
 */
globus_result_t
globus_gass_copy_attr_init(
    globus_gass_copy_attr_t * attr)
{
    globus_object_t * err;
    static char * myname="globus_gass_copy_attr_init";
    if(attr!=GLOBUS_NULL)
    {
	attr->ftp_attr = GLOBUS_NULL;
	attr->io = GLOBUS_NULL;
	attr->gass_requestattr = GLOBUS_NULL;
	return GLOBUS_SUCCESS;
    }
    else
    {
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: BAD_PARAMETER, attr is NULL",
	    myname);
	return globus_error_put(err);
    }
}

/**
 * Set the attributes for ftp/gsiftp transfers
 *
 * In order to specify attributes for ftp/gsiftp transfers, a
 * globus_ftp_client_operationattr_t should be initialized and its values
 * set using the appropriate globus_ftp_client_operationattr_* functions.  The
 * globus_ftp_client_operationattr_t * can then be passed to the
 * globus_gass_copy_attr_t via this function.
 *
 * @param attr
 *      A globus_gass_copy attribute structure
 * @param ftp_attr
 *      The ftp/gsiftp attributes to be used
 *
 * @return
 *       This function returns GLOBUS_SUCCESS if successful, or a
 *       globus_result_t indicating the error that occurred.
 *
 * @see globus_gass_copy_attr_init(),
 *      globus_gass_copy_attr_set_gass(),
 *      globus_gass_copy_attr_set_io(),
 *      globus_gass_copy_get_url_mode(),
 *      globus_ftp_client_operationattr_*
 */
globus_result_t
globus_gass_copy_attr_set_ftp(
    globus_gass_copy_attr_t * attr,
    globus_ftp_client_operationattr_t * ftp_attr)
{
    globus_object_t * err;
    static char * myname="globus_gass_copy_attr_set_ftp";

    if(attr != GLOBUS_NULL)
    {
	attr->ftp_attr = ftp_attr;
	return GLOBUS_SUCCESS;
    }
    else
    {
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: BAD_PARAMETER, attr is NULL",
	    myname);
	return globus_error_put(err);
    }
} /* globus_gass_copy_attr_set_ftp() */


/**
 * Set the attributes for file transfers
 *
 * In order to specify attributes for file transfers, a globus_io_attr_t should
 * be initialized and its values set using the appropriate globus_io_attr_*
 * functions.  The globus_io_attr_t can then be passed to the
 * globus_gass_copy_attr_t via this function.
 *
 * @param attr
 *      A globus_gass_copy attribute structure
 * @param io_attr
 *      The file attributes to be used
 *
 * @return
 *      This function returns GLOBUS_SUCCESS if successful, or a
 *      globus_result_t indicating the error that occurred.
 *
 * @see globus_gass_copy_attr_init(),
 *      globus_gass_copy_attr_set_gass(),
 *      globus_gass_copy_attr_set_ftp(),
 *      globus_gass_copy_get_url_mode(),
 *      globus_io_attr_*
 */
globus_result_t
globus_gass_copy_attr_set_io(
    globus_gass_copy_attr_t * attr,
    globus_io_attr_t * io_attr)
{
    globus_object_t * err;
    static char * myname="globus_gass_copy_attr_set_io";
    if(attr != GLOBUS_NULL)
    {
	attr->io = io_attr;
	return GLOBUS_SUCCESS;
    }
    else
    {
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: BAD_PARAMETER, attr is NULL",
	    myname);
	return globus_error_put(err);
    }
} /* globus_gass_copy_attr_set_io() */

/**
 * Set the attributes for http/https transfers
 *
 * In order to specify attributes for http/https transfers, a
 * globus_gass_transfer_requestattr_t should be initialized and its values
 * set using the appropriate globus_gass_transfer_requestattr_* functions.
 * The globus_gass_transfer_requestattr_t can then be passed to the
 * globus_gass_copy_attr_t via this function.
 *
 * @param attr
 *      A globus_gass_copy attribute structure
 * @param io_attr
 *      The http/https attributes to be used
 *
 * @return
 *      This function returns GLOBUS_SUCCESS if successful, or a
 *      globus_result_t indicating the error that occurred.
 *
 * @see globus_gass_copy_attr_init(),
 *      globus_gass_copy_attr_set_io(),
 *      globus_gass_copy_attr_set_ftp(),
 *      globus_gass_copy_get_url_mode(),
 *      globus_gass_transfer_requestattr_*
 */
globus_result_t
globus_gass_copy_attr_set_gass(
    globus_gass_copy_attr_t * attr,
    globus_gass_transfer_requestattr_t * gass_attr)
{
    globus_object_t * err;
    static char * myname="globus_gass_copy_attr_set_gass";
    if(attr != GLOBUS_NULL)
    {
	attr->gass_requestattr = gass_attr;
	return GLOBUS_SUCCESS;
    }
    else
    {
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: BAD_PARAMETER, attr is NULL",
	    myname);
	return globus_error_put(err);
    }
}



/**
 * Classify the URL schema into the transfer method that will be used to do
 * the actual tranfer.
 *
 * This function enables the user to determine what protocol will be used to
 * transfer data to/from a particular url.  This information can then be used
 * to specify the appropriate attributes when initiating a transfer.
 *
 * @param url
 *      The URL for schema checking
 * @param mode
 *      the filled in schema type of the URL param
 *
 * @return
 *      This function returns GLOBUS_SUCCESS if successful, or a
 *      globus_result_t indicating the error that occurred.
 *
 * @see globus_gass_copy_attr_init(),
 *      globus_gass_copy_attr_set_io(),
 *      globus_gass_copy_attr_set_ftp(),
 *      globus_gass_copy_set_gass()
 */
globus_result_t
globus_gass_copy_get_url_mode(
    char * url,
    globus_gass_copy_url_mode_t * mode)
{
    globus_url_t url_info;
    int rc;
    globus_object_t * err;
    static char * myname="globus_gass_copy_get_url_mode";

    if ((rc = globus_url_parse(url, &url_info)) != GLOBUS_SUCCESS)
    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "copy_url_mode(): globus_url_parse returned !GLOBUS_SUCCESS for url: %s\n", url);
#endif
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: globus_url_parse returned error code: %d for url: %s",
	    myname,
	    rc,
	    url);

	return globus_error_put(err);
    }

    if ( (url_info.scheme_type == GLOBUS_URL_SCHEME_FTP) ||
	 (url_info.scheme_type == GLOBUS_URL_SCHEME_GSIFTP) )
    {
	*mode = GLOBUS_GASS_COPY_URL_MODE_FTP;
    }
    else if ( (url_info.scheme_type == GLOBUS_URL_SCHEME_HTTP) ||
              (url_info.scheme_type == GLOBUS_URL_SCHEME_HTTPS) )
    {
	*mode = GLOBUS_GASS_COPY_URL_MODE_GASS;
    }
    else if ( (url_info.scheme_type == GLOBUS_URL_SCHEME_FILE))
    {
	*mode = GLOBUS_GASS_COPY_URL_MODE_IO;
    }
    else
    {
	*mode = GLOBUS_GASS_COPY_URL_MODE_UNSUPPORTED;
    }

    globus_url_destroy(&url_info);

    return GLOBUS_SUCCESS;
} /* globus_l_gass_copy_get_url_mode() */

/**
 * Register a performance information callback
 *
 * Use this to register a performance information callback.
 * You change or set to GLOBUS_NULL the callback any time a transfer is not
 * occurring.
 *
 * @param handle
 *        an initialized gass copy handle for which you would like to
 *        see performance info
 *
 * @param callback
 *        the performance callback
 *
 * @param user_arg
 *        a user pointer that will be passed to all callbacks for a given
 *         handle
 *
 * @return
 *        - GLOBUS_SUCCESS
 *        - error on a NULL or busy handle
 *
 * @see globus_gass_copy_performance_cb_t
 */

globus_result_t
globus_gass_copy_register_performance_cb(
    globus_gass_copy_handle_t *             handle,
    globus_gass_copy_performance_cb_t       callback,
    void *                                  user_arg)
{
    globus_result_t                         result;
    static const char *                     myname =
        "globus_gass_copy_register_performance_cb";

    if(handle == GLOBUS_NULL)
    {
	return globus_error_put(
	    globus_error_construct_string(
        	    GLOBUS_GASS_COPY_MODULE,
        	    GLOBUS_NULL,
        	    "[%s]: BAD_PARAMETER, handle is NULL",
        	    myname));
    }

    if(handle->status > GLOBUS_GASS_COPY_STATUS_NONE &&
        handle->status < GLOBUS_GASS_COPY_STATUS_DONE_SUCCESS)
    {
        return globus_error_put(
	    globus_error_construct_string(
        	    GLOBUS_GASS_COPY_MODULE,
        	    GLOBUS_NULL,
        	    "[%s]: There is a transfer active on this handle",
        	    myname));
    }

    if(callback == GLOBUS_NULL)
    {
        if(handle->performance)
        {
            globus_ftp_client_throughput_plugin_destroy(
                &handle->performance->ftp_perf_plugin);
            globus_mutex_destroy(&handle->performance->lock);
            globus_free(handle->performance);
            handle->performance = GLOBUS_NULL;
        }

        return GLOBUS_SUCCESS;
    }

    if(handle->performance == GLOBUS_NULL)
    {
        handle->performance = (globus_gass_copy_perf_info_t *)
            globus_malloc(sizeof(globus_gass_copy_perf_info_t));

        if(handle->performance == GLOBUS_NULL)
        {
            return globus_error_put(
	        globus_error_construct_string(
        	    GLOBUS_GASS_COPY_MODULE,
        	    GLOBUS_NULL,
        	    "[%s]: Memory allocation error",
        	    myname));
        }

        handle->performance->copy_handle = handle;
        handle->performance->saved_dest_attr = GLOBUS_FALSE;
        handle->performance->saved_source_attr = GLOBUS_FALSE;
        handle->performance->dest_ftp_attr = GLOBUS_NULL;
        handle->performance->source_ftp_attr = GLOBUS_NULL;

        result = globus_ftp_client_throughput_plugin_init(
            &handle->performance->ftp_perf_plugin,
            GLOBUS_NULL,
            GLOBUS_NULL,
            globus_l_gass_copy_perf_ftp_cb,
            GLOBUS_NULL,
            handle->performance);

        if(result != GLOBUS_SUCCESS)
        {
            globus_free(handle->performance);
            handle->performance = GLOBUS_NULL;
            return result;
        }

        globus_mutex_init(&handle->performance->lock, GLOBUS_NULL);
    }

    handle->performance->callback = callback;
    handle->performance->user_arg = user_arg;

    return GLOBUS_SUCCESS;
}

#ifndef GLOBUS_DONT_DOCUMENT_INTERNAL

static
void
globus_l_gass_copy_perf_local_cb(
    const globus_abstime_t *                timenow,
    const globus_abstime_t *                time_stop,
    void *                                  user_arg)
{
    globus_gass_copy_perf_info_t *          perf_info;
    float                                   instantaneous_throughput;
    float                                   avg_throughput;
    double                                  time_now;
    globus_off_t                            bytes_now;
    double                                  time_elapsed;
    struct timeb                            timebuf;
    globus_gass_copy_handle_t *             handle;
    globus_gass_copy_performance_cb_t       callback;
    
    perf_info = (globus_gass_copy_perf_info_t *) user_arg;

    globus_mutex_lock(&perf_info->lock);
    {
        ftime(&timebuf);
        time_now = timebuf.time + (timebuf.millitm / 1000.0);
        bytes_now = perf_info->live_bytes;
    
        time_elapsed = time_now - perf_info->prev_time;
        if(time_elapsed < 0.1)
        {
            /* shouldnt be possible (callback delay is 2 secs) */
            time_elapsed = 0.1;
        }
    
        instantaneous_throughput =
            (bytes_now - perf_info->prev_bytes) /
            time_elapsed;
    
        time_elapsed = time_now - perf_info->start_time;
        if(time_elapsed < 0.1)
        {
            /* shouldnt be possible (callback delay is 2 secs) */
            time_elapsed = 0.1;
        }
    
        avg_throughput =
            bytes_now /
            time_elapsed;
    
        perf_info->prev_time = time_now;
        perf_info->prev_bytes = bytes_now;
        
        handle = perf_info->copy_handle;
        user_arg = perf_info->user_arg;
        
        callback = perf_info->callback;
    }
    globus_mutex_unlock(&perf_info->lock);

    callback(
        user_arg,
        handle,
        bytes_now,
        instantaneous_throughput,
        avg_throughput);
}

static
void
globus_l_gass_copy_perf_ftp_cb(
    void *                                  user_arg,
    globus_ftp_client_handle_t *            handle,
    globus_off_t                            bytes,
    float                                   instantaneous_throughput,
    float                                   avg_throughput)
{
    globus_gass_copy_perf_info_t *          perf_info;

    perf_info = (globus_gass_copy_perf_info_t *) user_arg;

    perf_info->callback(
        perf_info->user_arg,
        perf_info->copy_handle,
        bytes,
        instantaneous_throughput,
        avg_throughput);
}

static
void
globus_l_gass_copy_perf_setup_local_callback(
    globus_gass_copy_perf_info_t *          perf_info)
{
    globus_reltime_t                        delay_time;
    globus_reltime_t                        period_time;
    struct timeb                            timebuf;

    ftime(&timebuf);
    perf_info->start_time = timebuf.time + (timebuf.millitm / 1000.0);
    perf_info->prev_time = perf_info->start_time;
    perf_info->prev_bytes = 0;
    perf_info->live_bytes = 0;

    GlobusTimeReltimeSet(delay_time, 2, 0);
    GlobusTimeReltimeSet(period_time, 2, 0);
    globus_callback_register_periodic(
        &perf_info->local_cb_handle,
        &delay_time,
        &period_time,
        globus_l_gass_copy_perf_local_cb,
        perf_info);
}

static
void
globus_l_gass_copy_perf_setup_ftp_callback(
    globus_gass_copy_perf_info_t *          perf_info)
{
    globus_ftp_client_handle_add_plugin(
        &perf_info->copy_handle->ftp_dest_handle,
        &perf_info->ftp_perf_plugin);
}

static
void
globus_l_gass_copy_perf_cancel_local_callback(
    globus_gass_copy_perf_info_t *          perf_info)
{
    globus_callback_unregister(
        perf_info->local_cb_handle, GLOBUS_NULL, GLOBUS_NULL, GLOBUS_NULL);
}

static
void
globus_l_gass_copy_perf_cancel_ftp_callback(
    globus_gass_copy_perf_info_t *          perf_info)
{
    globus_ftp_client_handle_remove_plugin(
        &perf_info->copy_handle->ftp_dest_handle,
        &perf_info->ftp_perf_plugin);

    if(perf_info->saved_dest_attr)
    {
        if(perf_info->copy_handle->state &&
            perf_info->copy_handle->state->dest.attr)
        {
            globus_ftp_client_operationattr_destroy(
                perf_info->copy_handle->state->dest.attr->ftp_attr);

            perf_info->copy_handle->state->dest.attr->ftp_attr =
                perf_info->dest_ftp_attr;
        }
        else
        {
            globus_ftp_client_operationattr_destroy(
                perf_info->dest_ftp_attr);
        }

        perf_info->saved_dest_attr = GLOBUS_FALSE;
        perf_info->dest_ftp_attr = GLOBUS_NULL;
    }

    if(perf_info->saved_source_attr)
    {
        if(perf_info->copy_handle->state &&
            perf_info->copy_handle->state->source.attr)
        {
            globus_ftp_client_operationattr_destroy(
                perf_info->copy_handle->state->source.attr->ftp_attr);

            perf_info->copy_handle->state->source.attr->ftp_attr =
                perf_info->source_ftp_attr;
        }
        else
        {
            globus_ftp_client_operationattr_destroy(
                perf_info->source_ftp_attr);
        }

        perf_info->saved_source_attr = GLOBUS_FALSE;
        perf_info->source_ftp_attr = GLOBUS_NULL;
    }
}

void
globus_l_gass_copy_gass_setup_callback(
    void * callback_arg,
    globus_gass_transfer_request_t request);

void
globus_l_gass_copy_read_from_queue(
    globus_gass_copy_handle_t * handle);

globus_result_t
globus_l_gass_copy_register_read(
    globus_gass_copy_handle_t * handle,
    globus_byte_t * buffer);

void
globus_l_gass_copy_gass_read_callback(
    void *                          callback_arg,
    globus_gass_transfer_request_t  request,
    globus_byte_t *                 bytes,
    globus_size_t                   nbytes,
    globus_bool_t                   last_data);

void
globus_l_gass_copy_io_read_callback(
    void *                          callback_arg,
    globus_io_handle_t *            io_handle,
    globus_result_t                 result,
    globus_byte_t *                 bytes,
    globus_size_t                   nbytes);

void
globus_l_gass_copy_ftp_read_callback(
    void *                          callback_arg,
    globus_ftp_client_handle_t *    handle,
    globus_object_t *               error,
    globus_byte_t *                 bytes,
    globus_size_t                   nbytes,
    globus_off_t                    offset,
    globus_bool_t		    eof);

globus_result_t
globus_l_gass_copy_io_setup_get(
    globus_gass_copy_handle_t * handle);

globus_result_t
globus_l_gass_copy_io_setup_put(
    globus_gass_copy_handle_t * handle);

globus_result_t
globus_l_gass_copy_ftp_setup_get(
    globus_gass_copy_handle_t * handle);

globus_result_t
globus_l_gass_copy_ftp_setup_put(
    globus_gass_copy_handle_t * handle);

void
globus_l_gass_copy_ftp_get_done_callback(
    void * callback_arg,
    globus_ftp_client_handle_t * handle,
    globus_object_t *	       error);

void
globus_l_gass_copy_ftp_put_done_callback(
    void * callback_arg,
    globus_ftp_client_handle_t * handle,
    globus_object_t *	       error);

void
globus_l_gass_copy_io_cancel_callback(
    void * callback_arg,
    globus_io_handle_t * handle,
    globus_result_t result);

void
globus_l_gass_copy_gass_transfer_cancel_callback(
    void * callback_arg,
    globus_gass_transfer_request_t request);

void
globus_l_gass_copy_write_from_queue(
    globus_gass_copy_handle_t * handle);

globus_result_t
globus_l_gass_copy_register_write(
    globus_gass_copy_handle_t * handle,
    globus_i_gass_copy_buffer_t * buffer_entry);

void
globus_l_gass_copy_gass_write_callback(
    void *                          callback_arg,
    globus_gass_transfer_request_t  request,
    globus_byte_t *                 bytes,
    globus_size_t                   nbytes,
    globus_bool_t                   last_data);

void
globus_l_gass_copy_io_write_callback(
    void *                callback_arg,
    globus_io_handle_t *  io_handle,
    globus_result_t       result,
    globus_byte_t *       bytes,
    globus_size_t         nbytes);

void
globus_l_gass_copy_ftp_write_callback(
    void *                       callback_arg,
    globus_ftp_client_handle_t * handle,
    globus_object_t *            error,
    globus_byte_t *              bytes,
    globus_size_t                nbytes,
    globus_off_t                 offset,
    globus_bool_t		 eof);

globus_result_t
globus_i_gass_copy_attr_duplicate(globus_gass_copy_attr_t ** attr);

globus_result_t
globus_l_gass_copy_target_cancel(globus_i_gass_copy_cancel_t * cancel_info);

void
globus_l_gass_copy_gass_transfer_cancel_callback(void * callback_arg,
                                       globus_gass_transfer_request_t request);

void
globus_l_gass_copy_io_cancel_callback(void * callback_arg,
                            globus_io_handle_t * handle,
                            globus_result_t result);

void
globus_l_gass_copy_generic_cancel(globus_i_gass_copy_cancel_t * cancel_info);

#endif /* GLOBUS_DONT_DOCUMENT_INTERNAL */

/**
 * Get the status code of the current transfer.
 *
 * Get the status of the last transfer to be initiated using the given handle.
 * Only one transfer can be active on a handle at a given time, therefore
 * new transfers may only be initiated when the current status is one of the
 * following: GLOBUS_GASS_COPY_STATUS_NONE,
 *            GLOBUS_GASS_COPY_STATUS_DONE_SUCCESS,
 *            GLOBUS_GASS_COPY_STATUS_DONE_FAILURE,
 *            GLOBUS_GASS_COPY_STATUS_DONE_CANCELLED
 *
 * @param handle
 *      A globus_gass_copy_handle
 * @param status
 *      Will be one of the following:
 *     GLOBUS_GASS_COPY_STATUS_NONE
 *         (No transfers have been initiated using this handle.)
 *     GLOBUS_GASS_COPY_STATUS_PENDING
 *         (A transfer is currently being set up.)
 *     GLOBUS_GASS_COPY_STATUS_TRANSFER_IN_PROGRESS
 *         (There is currently a transfer in progress.)
 *     GLOBUS_GASS_COPY_STATUS_CANCEL
 *         (The last transfer initiated using this handle has been cancelled by
 *          the user before completing, and is in the process of being
 *          cleaned up.)
 *     GLOBUS_GASS_COPY_STATUS_FAILURE
 *         (The last transfer initiated using this handle failed, and is in the
 *          process of being cleaned up.)
 *     GLOBUS_GASS_COPY_STATUS_DONE_SUCCESS
 *         (The last transfer initiated using this handle has completed
 *          successfully.)
 *     GLOBUS_GASS_COPY_STATUS_DONE_FAILURE
 *         (The last transfer initiated using this handle failed and has
 *          finished cleaning up.)
  *     GLOBUS_GASS_COPY_STATUS_DONE_CANCELLED
 *         (The last transfer initiated using this handle was cancelled
 *          and has finished cleaning up.)
 * @return
 *       This function returns GLOBUS_SUCCESS if successful, or a
 *       globus_result_t indicating the error that occurred.
 *
 */
globus_result_t
globus_gass_copy_get_status(
    globus_gass_copy_handle_t * handle,
    globus_gass_copy_status_t *status)
{
    globus_object_t * err;
    static char * myname="globus_gass_copy_get_status";
    if(handle != GLOBUS_NULL)
    {
	switch(handle->status)
	{
	case GLOBUS_GASS_COPY_STATUS_NONE:
	    *status = GLOBUS_GASS_COPY_STATUS_NONE;
	    break;
	case GLOBUS_GASS_COPY_STATUS_INITIAL:
	case GLOBUS_GASS_COPY_STATUS_SOURCE_READY:
	    *status = GLOBUS_GASS_COPY_STATUS_PENDING;
	    break;
	case GLOBUS_GASS_COPY_STATUS_TRANSFER_IN_PROGRESS:
	case GLOBUS_GASS_COPY_STATUS_READ_COMPLETE:
	case GLOBUS_GASS_COPY_STATUS_WRITE_COMPLETE:
	case GLOBUS_GASS_COPY_STATUS_DONE:
	    *status = GLOBUS_GASS_COPY_STATUS_TRANSFER_IN_PROGRESS;
	    break;
	case GLOBUS_GASS_COPY_STATUS_FAILURE:
	    *status = GLOBUS_GASS_COPY_STATUS_FAILURE;
	    break;
	case GLOBUS_GASS_COPY_STATUS_CANCEL:
	    *status = GLOBUS_GASS_COPY_STATUS_CANCEL;
	    break;
	case GLOBUS_GASS_COPY_STATUS_DONE_SUCCESS:
	    *status = GLOBUS_GASS_COPY_STATUS_DONE_SUCCESS;
	    break;
	case GLOBUS_GASS_COPY_STATUS_DONE_FAILURE:
	    *status = GLOBUS_GASS_COPY_STATUS_DONE_FAILURE;
	    break;
	case GLOBUS_GASS_COPY_STATUS_DONE_CANCELLED:
	    *status = GLOBUS_GASS_COPY_STATUS_DONE_CANCELLED;
	    break;
	}
	return GLOBUS_SUCCESS;
    }
    else
    {
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: BAD_PARAMETER, handle is NULL",
	    myname);

	return globus_error_put(err);
    }
} /* globus_gass_copy_get_status() */

/**
 * Get the status string of the current transfer.
 *
 * Get the status of the last transfer to be initiated using the given handle.
 * Only one transfer can be active on a handle at a given time, therefore
 * new transfers may only be initiated when the current status is one of the
 * following: GLOBUS_GASS_COPY_STATUS_NONE,
 *            GLOBUS_GASS_COPY_STATUS_DONE_SUCCESS,
 *            GLOBUS_GASS_COPY_STATUS_DONE_FAILURE,
 *            GLOBUS_GASS_COPY_STATUS_DONE_CANCELLED
 *
 * @param handle
 *      A globus_gass_copy_handle
 *
 * @return
 *      Returns a pointer to a character string describing the current status
 */
const char *
globus_gass_copy_get_status_string(
    globus_gass_copy_handle_t * handle)
{
  globus_object_t * err;
  static char * myname="globus_gass_copy_get_status_string";
  globus_gass_copy_status_t status_code;


static char *
    globus_l_gass_copy_status_string[13] =
  {
    "GLOBUS_GASS_COPY_STATUS_NONE",
    "GLOBUS_GASS_COPY_STATUS_PENDING",
    "GLOBUS_GASS_COPY_STATUS_INITIAL",
    "GLOBUS_GASS_COPY_STATUS_SOURCE_READY",
    "GLOBUS_GASS_COPY_STATUS_TRANSFER_IN_PROGRESS",
    "GLOBUS_GASS_COPY_STATUS_READ_COMPLETE",
    "GLOBUS_GASS_COPY_STATUS_WRITE_COMPLETE",
    "GLOBUS_GASS_COPY_STATUS_DONE",
    "GLOBUS_GASS_COPY_STATUS_FAILURE",
    "GLOBUS_GASS_COPY_STATUS_CANCEL",
    "GLOBUS_GASS_COPY_STATUS_DONE_SUCCESS",
    "GLOBUS_GASS_COPY_STATUS_DONE_FAILURE",
    "GLOBUS_GASS_COPY_STATUS_DONE_CANCELLED",
  };


  if(handle != GLOBUS_NULL)
  {
    globus_gass_copy_get_status(handle, &status_code);
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    /* status = globus_l_gass_copy_status_string[status_code];*/
    globus_libc_fprintf(stderr, "globus_gass_copy_get_status_string, status_code = %d, "
         "status_string = %s\n", status_code, globus_l_gass_copy_status_string[status_code]);
#endif
    return(globus_l_gass_copy_status_string[status_code]);

  }
  else
  {
    err = globus_error_construct_string(
        GLOBUS_GASS_COPY_MODULE,
	GLOBUS_NULL,
	"[%s]: BAD_PARAMETER, handle is NULL",
	myname);

	return globus_error_put(err);
  }
}

#ifndef GLOBUS_DONT_DOCUMENT_INTERNAL

/* if either source or detination are ftp urls, wait for their callbacks to be
 * called before cleaning up and calling the user callback
 */
void
globus_l_gass_copy_wait_for_ftp_callbacks(
    globus_gass_copy_handle_t *handle)
{
    globus_i_gass_copy_monitor_t * source_monitor
	= &(handle->state->source.data.ftp.monitor);
    globus_i_gass_copy_monitor_t * dest_monitor
	= &(handle->state->dest.data.ftp.monitor);

#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "wait_for_ftp_callback(): starting\n");
#endif

    if(handle->state->source.mode == GLOBUS_GASS_COPY_URL_MODE_FTP)
    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "wait_for_ftp_callback(): waiting on source\n");
#endif
	globus_mutex_lock(&(source_monitor->mutex));
	while(!source_monitor->done)
	{
	    globus_cond_wait(&(source_monitor->cond), &(source_monitor->mutex));
	}
	globus_mutex_unlock(&(source_monitor->mutex));
    }

    if(handle->state && handle->state->dest.mode == GLOBUS_GASS_COPY_URL_MODE_FTP)
    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "wait_for_ftp_callback(): waiting on dest\n");
#endif
	globus_mutex_lock(&(dest_monitor->mutex));
	while(!dest_monitor->done)
	{
	    globus_cond_wait(&(dest_monitor->cond), &(dest_monitor->mutex));
	}
	globus_mutex_unlock(&(dest_monitor->mutex));
    }
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "wait_for_ftp_callback(): exiting\n");
#endif
}/* globus_l_gass_copy_wait_for_ftp_callbacks() */


/**
 * Populate the target transfer structures
 */
globus_result_t
globus_l_gass_copy_target_populate(
    globus_gass_copy_handle_t * handle,
    globus_i_gass_copy_target_t * target,
    globus_gass_copy_url_mode_t * url_mode,
    char * url,
    globus_gass_copy_attr_t * attr)
{
    globus_object_t * err;
    globus_gass_copy_attr_t * tmp_attr;
    static char * myname="globus_l_gass_copy_target_populate";
    /* initialize the target mutex */
    globus_mutex_init(&(target->mutex), GLOBUS_NULL);

    target->n_pending = 0;
    target->n_complete = 0;
    target->status = GLOBUS_I_GASS_COPY_TARGET_INITIAL;
    target->free_ftp_attr = GLOBUS_FALSE;

    if(attr == GLOBUS_NULL)
    {
	target->free_attr = GLOBUS_TRUE;
	tmp_attr = (globus_gass_copy_attr_t *)
                   globus_libc_malloc(sizeof(globus_gass_copy_attr_t));

	if(tmp_attr == GLOBUS_NULL)
	{
	    err = globus_error_construct_string(
		GLOBUS_GASS_COPY_MODULE,
		GLOBUS_NULL,
		"[%s]: failed malloc a globus_gass_copy_attr_t structure successfully",
		myname);
	    return globus_error_put(err);
	}

	globus_gass_copy_attr_init(tmp_attr);

	attr = tmp_attr;
    }
    else
	target->free_attr = GLOBUS_FALSE;

    target->mode = *url_mode;
    switch (target->mode)
    {
    case GLOBUS_GASS_COPY_URL_MODE_FTP:

	/* target->mode = *url_mode; */
	globus_mutex_init(&(target->data.ftp.monitor.mutex), GLOBUS_NULL);
	globus_cond_init(&(target->data.ftp.monitor.cond), GLOBUS_NULL);
	target->data.ftp.monitor.done = GLOBUS_FALSE;
	target->url = globus_libc_strdup(url);
	target->attr = attr;
	/* FIXX n_simultaneous should be pulled from attributes, or something */
	if(attr->ftp_attr)
	{
	    globus_ftp_control_parallelism_t  tmp_parallelism;
	    globus_ftp_client_operationattr_get_parallelism(attr->ftp_attr, &tmp_parallelism);
	    if(tmp_parallelism.mode == GLOBUS_FTP_CONTROL_PARALLELISM_FIXED)
	    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
		globus_libc_fprintf(stderr, "tmp_parallelism.mode== GLOBUS_FTP_CONTROL_PARALLELISM_FIXED, tmp_parallelism.fixed.size = %d\n", tmp_parallelism.fixed.size);
#endif
		target->n_simultaneous = tmp_parallelism.fixed.size;
		/*target->n_simultaneous = 1;*/
	    }
	    else
		target->n_simultaneous = 1;
	}
	else
	{
	    target->n_simultaneous = 1;
	    /*globus_libc_fprintf(stderr, "****  target->n_simultaneous: %d\n",target->n_simultaneous); */
	}
	break;

    case GLOBUS_GASS_COPY_URL_MODE_GASS:

	/*target->mode = GLOBUS_I_GASS_COPY_TARGET_MODE_GASS; */
	target->url = globus_libc_strdup(url);
	target->attr = attr;
	target->n_simultaneous = 1;
	break;

    case GLOBUS_GASS_COPY_URL_MODE_IO:

	/*target->mode = GLOBUS_I_GASS_COPY_TARGET_MODE_IO;*/
	target->url = globus_libc_strdup(url);
	target->attr = attr;
	target->data.io.free_handle = GLOBUS_TRUE;
	target->data.io.seekable = GLOBUS_TRUE;
	target->data.io.handle = GLOBUS_NULL;
	target->n_simultaneous = 1;

	break;

    case GLOBUS_GASS_COPY_URL_MODE_UNSUPPORTED:
	/* something went horribly wrong */
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: %s: GLOBUS_GASS_COPY_URL_MODE_UNSUPPORTED",
	    myname,
	    url);
	return globus_error_put(err);

	break;
    }

    /* setup the queue
     */
    if (globus_fifo_init(&(target->queue)) != GLOBUS_SUCCESS)
    {
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: failed to initialize fifo successfully",
	    myname);
	return globus_error_put(err);
    }

    return GLOBUS_SUCCESS;
} /* globus_l_gass_copy_target_populate() */

globus_result_t
globus_l_gass_copy_io_target_populate(
    globus_gass_copy_handle_t * handle,
    globus_i_gass_copy_target_t * target,
    globus_io_handle_t * io_handle)
{
    globus_object_t * err;
    static char * myname="globus_l_gass_copy_io_target_populate";

    target->free_attr = GLOBUS_FALSE;
    /* initialize the target mutex */
    globus_mutex_init(&(target->mutex), GLOBUS_NULL);

    target->data.io.handle = io_handle;

    target->n_pending = 0;
    target->n_complete = 0;
    target->status = GLOBUS_I_GASS_COPY_TARGET_INITIAL;

    target->mode = GLOBUS_GASS_COPY_URL_MODE_IO;

    target->data.io.free_handle = GLOBUS_FALSE;
    if(globus_io_get_handle_type(io_handle) == GLOBUS_IO_HANDLE_TYPE_FILE)
	target->data.io.seekable = GLOBUS_TRUE;
    else
	target->data.io.seekable = GLOBUS_FALSE;
    target->n_simultaneous = 1;

    /* setup the queue
     */
    if (globus_fifo_init(&(target->queue)) != GLOBUS_SUCCESS)
    {
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: failed to initialize fifo successfully",
	    myname);
	return globus_error_put(err);
    }

    return GLOBUS_SUCCESS;
} /* globus_l_gass_copy_io_target_populate() */


/**
 * Clean up the target transfer structures, freeing any memory that was
 * allocated
 */
globus_result_t
globus_l_gass_copy_target_destroy(
    globus_i_gass_copy_target_t * target)
{
    globus_i_gass_copy_buffer_t *  buffer_entry;
    /* empty and free the queue */
    while(!globus_fifo_empty(&(target->queue)))
    {
	buffer_entry = globus_fifo_dequeue(&(target->queue));
	globus_libc_free(buffer_entry->bytes);
	globus_libc_free(buffer_entry);
    }
    globus_fifo_destroy(&(target->queue));
    /* clean up the mutex */
    globus_mutex_destroy(&(target->mutex));
    /* free up the attr, if we allocated it */
    if(target->free_attr == GLOBUS_TRUE)
    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "target_destroy(): freeing the target attr\n");
#endif
	if(target->mode == GLOBUS_GASS_COPY_URL_MODE_FTP &&
	   target->free_ftp_attr == GLOBUS_TRUE)
	    globus_libc_free(target->attr->ftp_attr);
	globus_libc_free(target->attr);
    }

    switch(target->mode)
    {
    case GLOBUS_GASS_COPY_URL_MODE_FTP:
	/* once parallel reads/writes are possible, will have to potentially
         * free the attr, if parallelism is turned off by the library
         */

	globus_libc_free((target->url));

	globus_mutex_destroy(&(target->data.ftp.monitor.mutex));
	globus_cond_destroy(&(target->data.ftp.monitor.cond));

	break;

    case GLOBUS_GASS_COPY_URL_MODE_GASS:
	globus_libc_free((target->url));
	break;

    case GLOBUS_GASS_COPY_URL_MODE_IO:
	if(target->data.io.free_handle == GLOBUS_TRUE)
	{
	    if(target->data.io.handle)
	    {
	        globus_libc_free((target->data.io.handle));
	    }
	    globus_libc_free((target->url));

	}
	break;
    }
    return GLOBUS_SUCCESS;
} /* gloubs_l_gass_copy_target_destroy() */

/**
 * instantiate state structure
 */
globus_result_t
globus_l_gass_copy_state_new(
    globus_gass_copy_handle_t *handle)
{
    globus_object_t * err;
    static char * myname="globus_l_gass_copy_state_new";

    globus_gass_copy_state_t ** tmp_state = &(handle->state);
    *tmp_state = (globus_gass_copy_state_t *)
	globus_libc_malloc(sizeof(globus_gass_copy_state_t));

    if(tmp_state == GLOBUS_NULL)
    {
	handle->status = GLOBUS_GASS_COPY_STATUS_FAILURE;
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: failed to malloc a globus_gass_copy_state_t successfully",
	    myname);

	return globus_error_put(err);
    }

    handle->status = GLOBUS_GASS_COPY_STATUS_INITIAL;
    handle->err = GLOBUS_SUCCESS;

    /* initialize the monitor */
    globus_mutex_init(&((*tmp_state)->monitor.mutex), GLOBUS_NULL);
    globus_cond_init(&((*tmp_state)->monitor.cond), GLOBUS_NULL);
    (*tmp_state)->monitor.done = GLOBUS_FALSE;

    (*tmp_state)->monitor.err = GLOBUS_NULL;
    (*tmp_state)->monitor.use_err = GLOBUS_FALSE;

    globus_mutex_init(&((*tmp_state)->mutex), GLOBUS_NULL);

    return GLOBUS_SUCCESS;
} /* globus_l_gass_copy_state_new() */

/**
 * free state structure
 */
globus_result_t
globus_l_gass_copy_state_free(
    globus_gass_copy_state_t * state)
{
    if(!state)
    {
        return GLOBUS_SUCCESS;
    }

#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr, "globus_l_gass_copy_state_free(): freeing up the state\n");
#endif
    /* clean  up the monitor */
    globus_mutex_destroy(&(state->monitor.mutex));
    globus_cond_destroy(&(state->monitor.cond));

    globus_mutex_destroy(&(state->mutex));

    /* clean  up the source target */
    globus_l_gass_copy_target_destroy(&(state->source));

    /* clean  up the destination target */
    globus_l_gass_copy_target_destroy(&(state->dest));

    /* free up the state */

    globus_libc_free(state);

    return GLOBUS_SUCCESS;

} /* globus_l_gass_copy_state_free() */

/**
 * Start the transfer.
 *
 * Based on the source and destination information in the state structure, start
 * the data transfer using the appropriate method - FTP, GASS, IO
 *
 * @param handle
 *        structure containing all the information required to perform data
 *        transfer from a source to a destination.
 *
 * @return
 *       This function returns GLOBUS_SUCCESS if successful, or a
 *       globus_result_t indicating the error that occurred.
 *
 * @retval GLOBUS_SUCCESS
 *         Descriptions
 * @retval GLOBUS_FAILRUE
 *
 * @see globus_gass_copy_xxx()
 */
globus_result_t
globus_l_gass_copy_transfer_start(
    globus_gass_copy_handle_t * handle)
{
    globus_gass_copy_state_t * state = handle->state;
    globus_result_t result = GLOBUS_SUCCESS;
    int rc;
    globus_object_t * err;
    globus_i_gass_copy_monitor_t        monitor;
    static char * myname="globus_l_gass_copy_transfer_start";

#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr, "made it to globus_l_gass_copy_transfer_start()\n");
#endif

    if (   (state->source.mode
	    == GLOBUS_GASS_COPY_URL_MODE_FTP)
	   && (   (   (state->dest.mode
		       == GLOBUS_GASS_COPY_URL_MODE_GASS) )
		  || (   (state->dest.mode
			  == GLOBUS_GASS_COPY_URL_MODE_IO)
			 && (!state->dest.data.io.seekable) ) ) )
    {
	/*
	 * If the source stream is ftp, this means it is capable
	 * of supporting multiple data channels and handing back
	 * data block in an arbitrary order.
	 *
	 * If the destination stream can only handle sequential
	 * writes of the data, then disable the multiple data
	 * channel support in ftp
	 */
	if(state->source.attr->ftp_attr != GLOBUS_NULL)
	{
	    globus_ftp_control_parallelism_t  tmp_parallelism;

	    globus_ftp_client_operationattr_get_parallelism(state->source.attr->ftp_attr, &tmp_parallelism);

	    if(tmp_parallelism.mode != GLOBUS_FTP_CONTROL_PARALLELISM_NONE)
	    {
		globus_gass_copy_attr_t * new_attr;
		globus_ftp_client_operationattr_t * new_ftp_attr;

		new_attr = (globus_gass_copy_attr_t *)
		    globus_libc_malloc(sizeof(globus_gass_copy_attr_t));
		globus_gass_copy_attr_init(new_attr);

		new_ftp_attr = (globus_ftp_client_operationattr_t *)
		    globus_libc_malloc(sizeof(globus_ftp_client_operationattr_t));

		globus_ftp_client_operationattr_copy(new_ftp_attr, (state->source.attr->ftp_attr));
		tmp_parallelism.mode = GLOBUS_FTP_CONTROL_PARALLELISM_NONE;
		globus_ftp_client_operationattr_set_parallelism(new_ftp_attr, &tmp_parallelism);

		globus_gass_copy_attr_set_ftp(new_attr, new_ftp_attr);

		state->source.attr = new_attr;

		state->source.free_attr = GLOBUS_TRUE;
		state->source.free_ftp_attr = GLOBUS_TRUE;
	    } /* if(tmp_parallelism.mode != GLOBUS_FTP_CONTROL_PARALLELISM_NONE) */
	} /* if(state->source.attr->ftp_attr != GLOBUS_NULL) */
	/*
	  state->source.attr.parallelism_info.mode =
	  GLOBUS_GSIFTP_CONTROL_PARALLELISM_NONE;
	  state->source.attr.striping_info.mode =
	  GLOBUS_GSIFTP_CONTROL_STRIPING_NONE;
	  */
	/*
	 * ftp -> gass_transfer:
	 *     turn off both parallel & striping
	 * ftp -> io:
	 *     globus_io_file_seek() can be used to deal with out of
	 *     order blocks
	 * gass_transfer, io -> *
	 *     The source data is serialized anyway, so do don't need
	 *     to worry about the destination.  An ftp destination can
	 *     use parallelism and/or striping if desired
	 */
    } /* source.mode == GLOBUS_GASS_COPY_URL_MODE_FTP, and dest is not seekable */

    /* depending on the mode, call the appropriate routine to start the
     * transfer
     */
    switch (state->source.mode)
    {
    case GLOBUS_GASS_COPY_URL_MODE_FTP:

	state->source.data.ftp.n_channels = 0;
	state->source.data.ftp.n_reads_posted = 0;
        state->source.data.ftp.handle = &(handle->ftp_source_handle);

        result = globus_l_gass_copy_ftp_setup_get(handle);

	break;

    case GLOBUS_GASS_COPY_URL_MODE_GASS:
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "transfer_start(): about to call globus_gass_transfer_register_get()\n");
#endif
	rc = globus_gass_transfer_register_get(
	    &(state->source.data.gass.request),
	    (state->source.attr->gass_requestattr),
	    state->source.url,
	    globus_l_gass_copy_gass_setup_callback,
	    (void *) handle);
/*
  FIXX - what happens if this is a referral?
  */
	if (rc != GLOBUS_SUCCESS)
	{
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	    globus_libc_fprintf(stderr, "transfer_start(): globus_gass_transfer_register_get returned !GLOBUS_SUCCESS\n");
#endif
	    err = globus_error_construct_string(
		GLOBUS_GASS_COPY_MODULE,
		GLOBUS_NULL,
		"[%s]: %s globus_gass_transfer_register_get returned an error code of: %d",
		myname,
		state->source.url,
		rc);
	    globus_i_gass_copy_set_error(handle, err);

	    result = globus_error_put(err);
	}
	break;

    case GLOBUS_GASS_COPY_URL_MODE_IO:

	result = globus_l_gass_copy_io_setup_get(handle);

	break;
    }

    if(result != GLOBUS_SUCCESS)
    {
	globus_i_gass_copy_set_error_from_result(handle, result);
	handle->status = GLOBUS_GASS_COPY_STATUS_FAILURE;
	return result;
    }

    /* wait for ok from the source */
    globus_mutex_lock(&(state->monitor.mutex));
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr, "transfer_start(): about to cond_wait() while source is setup\n");
#endif
    while(state->source.status == GLOBUS_I_GASS_COPY_TARGET_INITIAL)
    {
        globus_cond_wait(&state->monitor.cond,
			 &state->monitor.mutex);
    }
    globus_mutex_unlock(&state->monitor.mutex);

    if(handle->err)
    {
	handle->status = GLOBUS_GASS_COPY_STATUS_FAILURE;
	err = handle->err;
	handle->err = GLOBUS_NULL;
	return globus_error_put(err);
    }

    handle->status = GLOBUS_GASS_COPY_STATUS_SOURCE_READY;
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr, "transfer_start(): source is ready\n");
#endif
    /*
     * Now get the destination side ready
     */

    if(handle->performance)
    {
        if(state->dest.mode == GLOBUS_GASS_COPY_URL_MODE_FTP)
        {
            globus_l_gass_copy_perf_setup_ftp_callback(handle->performance);
        }
        else
        {
            globus_l_gass_copy_perf_setup_local_callback(handle->performance);
        }
    }

    switch (state->dest.mode)
    {
    case GLOBUS_GASS_COPY_URL_MODE_FTP:

	state->dest.data.ftp.n_channels = 0;
	state->dest.data.ftp.n_reads_posted = 0;

        state->dest.data.ftp.handle = &(handle->ftp_dest_handle);
	result = globus_l_gass_copy_ftp_setup_put(handle);
	break;

    case GLOBUS_GASS_COPY_URL_MODE_GASS:
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "transfer_start(): about to call globus_gass_transfer_register_put()\n");
#endif
        rc = globus_gass_transfer_register_put(
	    &(state->dest.data.gass.request),
	    (state->dest.attr->gass_requestattr),
	    state->dest.url,
	    GLOBUS_NULL,
	    globus_l_gass_copy_gass_setup_callback,
	    (void *) handle);

	if (rc != GLOBUS_SUCCESS)
	{
	    err = globus_error_construct_string(
		GLOBUS_GASS_COPY_MODULE,
		GLOBUS_NULL,
		"[%s]: %s globus_gass_transfer_register_put returned an error code of: %d",
		myname,
		state->dest.url,
		rc);
	    globus_i_gass_copy_set_error(handle, err);

	    result = globus_error_put(err);
	}

	break;

    case GLOBUS_GASS_COPY_URL_MODE_IO:
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "transfer_start(): about to call globus_l_gass_copy_io_setup_put()\n");
#endif
	result = globus_l_gass_copy_io_setup_put(handle);

	break;
    }

    if(result != GLOBUS_SUCCESS)
    {
        if(handle->performance)
        {
            if(state->dest.mode == GLOBUS_GASS_COPY_URL_MODE_FTP)
            {
                globus_l_gass_copy_perf_cancel_ftp_callback(handle->performance);
            }
            else
            {
                globus_l_gass_copy_perf_cancel_local_callback(handle->performance);
            }
        }

	handle->status = GLOBUS_GASS_COPY_STATUS_FAILURE;

#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "transfer_start(): error with setting up the dest\n");
#endif

        /* clean up the source side since it was already opened..... */
	globus_mutex_init(&monitor.mutex, GLOBUS_NULL);
	globus_cond_init(&monitor.cond, GLOBUS_NULL);
	monitor.done = GLOBUS_FALSE;
	monitor.err = GLOBUS_NULL;
	monitor.use_err = GLOBUS_FALSE;
        handle->user_callback = GLOBUS_NULL;
        globus_gass_copy_cancel(
	    handle,
	    globus_l_gass_copy_monitor_callback,
	    (void *) &monitor);
	/* wait for the cancel to complete before returning to user */
	globus_mutex_lock(&monitor.mutex);
	{
	    while(!monitor.done)
	    {
		globus_cond_wait(&monitor.cond, &monitor.mutex);
	    }
	}
	globus_mutex_unlock(&monitor.mutex);
	globus_mutex_destroy(&monitor.mutex);
	globus_cond_destroy(&monitor.cond);

	return result;
    }
    /* wait for ok from the dest */
    globus_mutex_lock(&(state->monitor.mutex));

#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr, "transfer_start(): about to cond_wait() while dest is setup\n");
#endif

    while(state->dest.status == GLOBUS_I_GASS_COPY_TARGET_INITIAL)
    {
        globus_cond_wait(&state->monitor.cond,
			 &state->monitor.mutex);
    }
    globus_mutex_unlock(&state->monitor.mutex);

    if(handle->err)
    {
	handle->status = GLOBUS_GASS_COPY_STATUS_FAILURE;

        /* clean up the source side since it was already opened..... */
        globus_gass_copy_cancel(handle, NULL, NULL);
	err = handle->err;
	handle->err = GLOBUS_NULL;
	return globus_error_put(err);
    }
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr, "transfer_start(): dest is ready, let's get goin'\n");
#endif
    /* both sides are ready, start the transfer */
    state->n_buffers = 0;
    state->max_buffers = state->source.n_simultaneous +
	state->dest.n_simultaneous;
    handle->status = GLOBUS_GASS_COPY_STATUS_TRANSFER_IN_PROGRESS;

    globus_l_gass_copy_read_from_queue(handle); /*start reading */
    return(GLOBUS_SUCCESS);
} /* globus_l_gass_copy_transfer_start() */

void
globus_l_gass_copy_read_from_queue(
    globus_gass_copy_handle_t * handle)
{
    globus_gass_copy_state_t * state = handle->state;
    globus_i_gass_copy_buffer_t *  buffer_entry;
    globus_byte_t * buffer = GLOBUS_NULL;
    globus_result_t result = GLOBUS_SUCCESS;
    globus_object_t * err;
    globus_bool_t do_the_read = GLOBUS_FALSE;
    static char * myname="globus_l_gass_copy_read_from_queue";

    while(1)
    {
	do_the_read = GLOBUS_FALSE;
	buffer_entry = GLOBUS_NULL;
	buffer = GLOBUS_NULL;

	globus_mutex_lock(&(state->source.mutex));
	{
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	    globus_libc_fprintf(stderr,
                    "read_from_queue(): n_pending= %d  n_simultaneous= %d\n",
                    state->source.n_pending,
                    state->source.n_simultaneous);
#endif

	    if(state->source.status == GLOBUS_I_GASS_COPY_TARGET_READY)
	    {/* the  source is ready (and not DONE, FAILURE, CANCEL) */
		if (((state->source.n_pending <
		      state->source.n_simultaneous) &&
		     !state->cancel))
		{ /* if there aren't too many reads outstanding,
                   * and we haven't canceled
                   */
		    if(!globus_fifo_empty(&(state->source.queue)))
		    {/* there's a buffer in the queue, use it */
			state->source.n_pending++;
			buffer_entry =
                            globus_fifo_dequeue(&(state->source.queue));
			buffer = buffer_entry->bytes;
			do_the_read = GLOBUS_TRUE;
		    } /* there's a buffer in the queue, use it */
		    else
		    {
                        globus_mutex_lock(&(state->mutex));
                        if(state->n_buffers < state->max_buffers)
                        {
                            state->n_buffers++;
                            state->source.n_pending++;
                            do_the_read = GLOBUS_TRUE;
                        }
                        globus_mutex_unlock(&(state->mutex));
#ifdef GLOBUS_I_GASS_COPY_DEBUG
		    globus_libc_fprintf(stderr, "read_from_queue: should create a new buffer\n");
#endif
		    } /* else (no buffer in the queue, we'll need to create one) */
		} /* (n_pending < n_simulatneous) && !cancel */

	    } /* if(state->source.status == GLOBUS_I_GASS_COPY_TARGET_READY) */
	} /* lock state->source */
	globus_mutex_unlock(&(state->source.mutex));


	if(do_the_read)
	{
	    if(buffer==GLOBUS_NULL)
	    { /* we need to create a buffer */
		buffer = globus_libc_malloc(handle->buffer_length);
#ifdef GLOBUS_I_GASS_COPY_DEBUG
		globus_libc_fprintf(stderr, "read_from_queue: new buffer was created: %d\n", buffer);
#endif
		if(buffer == GLOBUS_NULL)
		{
		    /* out of memory error */
		    err = globus_error_construct_string(
			GLOBUS_GASS_COPY_MODULE,
			GLOBUS_NULL,
			"[%s]: failed to malloc buffer of size %d",
			myname,
			handle->buffer_length);
		    globus_i_gass_copy_set_error(handle, err);
		    result = globus_error_put(err);
		} /* if(buffer == GLOBUS_NULL), the create failed*/
	    } /* if(buffer == GLOBUS_NULL), we need to create a buffer */

	    if(result==GLOBUS_SUCCESS)
	    { /* we have a valid buffer, register a read */
#ifdef GLOBUS_I_GASS_COPY_DEBUG
		globus_libc_fprintf(stderr, "read_from_queue: gonna register_read with buffer: %d\n", buffer);
#endif
		result = globus_l_gass_copy_register_read(
		    handle,
		    buffer);

		if(buffer_entry)
		    globus_libc_free(buffer_entry);
	    }

            if (result != GLOBUS_SUCCESS)
            {/* there was an error from above */
                state->cancel = GLOBUS_I_GASS_COPY_CANCEL_TRUE;
#ifdef GLOBUS_I_GASS_COPY_DEBUG
                globus_libc_fprintf(stderr, "read_from_queue(): there was an ERROR trying to register a read, call cancel\n");
#endif          
                globus_i_gass_copy_set_error_from_result(handle, result);
                globus_gass_copy_cancel(handle, NULL, NULL);
            }

	} /* if (do_the_read) */
	else
	    break;
    } /* while (1)  */
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	    globus_libc_fprintf(stderr, "read_from_queue(): returning\n");
#endif
} /* globus_l_gass_copy_read_from_queue() */


/**
 * register read
 *
 * Based on the mod of the source, register a read using the appropriate
 * data transfer method.
 *
 * @param handle
 *        structure containing all the information required to perform data
 *        transfer from a source to a destination.
 * @param buffer
 *        The buffer to be used to transfer the data.
 *
 * @return
 *       This function returns GLOBUS_SUCCESS if successful, or a
 *       globus_result_t indicating the error that occurred.
 *
 * @retval GLOBUS_SUCCESS
 *         Descriptions
 * @retval GLOBUS_FAILRUE
 *
 * @see globus_gass_copy_xxx()
 */
globus_result_t
globus_l_gass_copy_register_read(
    globus_gass_copy_handle_t * handle,
    globus_byte_t * buffer)
{
    globus_gass_copy_state_t * state = handle->state;
    globus_result_t result;
    int rc;
    globus_object_t * err;
    static char * myname="globus_l_gass_copy_register_read";

    switch (state->source.mode)
    {
    case GLOBUS_GASS_COPY_URL_MODE_FTP:
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "register_read():  calling globus_ftp_client_register_read()\n");
#endif
 	result = globus_ftp_client_register_read(
	    &(handle->ftp_source_handle),
	    buffer,
	    handle->buffer_length,
	    globus_l_gass_copy_ftp_read_callback,
	    (void *) handle);

	break;

    case GLOBUS_GASS_COPY_URL_MODE_GASS:
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "register_read():  calling globus_gass_transfer_receive_bytes()\n");
#endif
	rc = globus_gass_transfer_receive_bytes(
	    state->source.data.gass.request,
	    buffer,
	    handle->buffer_length,
	    handle->buffer_length,
	    globus_l_gass_copy_gass_read_callback,
	    (void *) handle);

	if (rc != GLOBUS_SUCCESS)
	{
	    /* figure out what the error is, and pass it back through the result             */
	    err = globus_error_construct_string(
		GLOBUS_GASS_COPY_MODULE,
		GLOBUS_NULL,
		"[%s]: globus_gass_transfer_receive_bytes returned error code: %d",
		myname,
		rc);
	    result = globus_error_put(err);
	}
	else result = GLOBUS_SUCCESS;

	break;

    case GLOBUS_GASS_COPY_URL_MODE_IO:

	result = globus_io_register_read(
	    state->source.data.io.handle,
	    buffer,
	    handle->buffer_length,
	    handle->buffer_length,
	    globus_l_gass_copy_io_read_callback,
	    (void *) handle);


	break;
    }

    return result;

} /* globus_l_gass_copy_register_read */

/*****************************************************************
 * setup callbacks
 *****************************************************************/


/**
 * GASS setup callback.
 *
 * This function is called after the connection attempt to the target
 * (e.g source or destination) has completed, failed, is a referral, ...
 *
 */
void
globus_l_gass_copy_gass_setup_callback(
    void * callback_arg,
    globus_gass_transfer_request_t  request)
{
    globus_gass_transfer_referral_t  referral;
    int rc;
    globus_object_t * err;
    char * current_url;
    char * denial_message;
    int denial_reason;
    static char * myname="globus_l_gass_copy_gass_setup_callback";

    globus_gass_copy_handle_t *  handle
	= (globus_gass_copy_handle_t *) callback_arg;
    globus_gass_copy_state_t * state = handle->state;


    globus_gass_transfer_request_status_t status;
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr, "globus_l_gass_copy_gass_setup_callback() called\n");
#endif
    status = globus_gass_transfer_request_get_status(request);

    switch(status)
    {
    case GLOBUS_GASS_TRANSFER_REQUEST_REFERRED:
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr,
                "request status == GLOBUS_GASS_TRANSFER_REQUEST_REFERRED\n");
#endif
	globus_gass_transfer_request_get_referral(request, &referral);
	globus_gass_transfer_request_destroy(request);

	if (handle->status == GLOBUS_GASS_COPY_STATUS_INITIAL)
	{
	    /* first setup the source with the register get
	     */
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	    globus_libc_fprintf(stderr, "REQUEST_REFERRED:  STATE_INITIAL\n");
#endif
	    current_url = globus_libc_strdup(state->source.url);
	    globus_libc_free(state->source.url);
	    state->source.url =globus_libc_strdup(
		globus_gass_transfer_referral_get_url(&referral, 0));

#ifdef GLOBUS_I_GASS_COPY_DEBUG
	    globus_libc_fprintf(stderr, "REQUEST_REFERRED: about to globus_gass_transfer_register_get() again with: %s\n",state->source.url);
#endif
	    if ( (rc = globus_gass_transfer_register_get(
		&(state->source.data.gass.request),
		(state->source.attr->gass_requestattr),
		state->source.url,
		globus_l_gass_copy_gass_setup_callback,
		(void *) handle)) != GLOBUS_SUCCESS )
	    {/* there was an error */
		globus_mutex_lock(&state->monitor.mutex);
#ifdef GLOBUS_I_GASS_COPY_DEBUG
		globus_libc_fprintf(stderr, "gass_setup_callback(): transfer_register_get() returned: %d\n", rc);
		if(rc==GLOBUS_GASS_TRANSFER_ERROR_BAD_URL)
		    globus_libc_fprintf(stderr, "rc == GLOBUS_GASS_ERROR_BAD_URL\n");
#endif
		err = globus_error_construct_string(
		    GLOBUS_GASS_COPY_MODULE,
		    GLOBUS_NULL,
		    "[%s]: the original source url: %s  was referred to: %s, for which globus_gass_transfer_register_get returned an error code of: %d",
		    myname,
		    current_url,
		    state->source.url,
		    rc);
		globus_i_gass_copy_set_error(handle, err);
		globus_libc_free(current_url);
		globus_gass_transfer_referral_destroy(&referral);
		goto wakeup_state;
	    }
	    globus_gass_transfer_referral_destroy(&referral);
	}
	else
	{
	    /* if the state is not INITIAL then assume the source is ready
	     * and that we are now setting up the destination with the register put
	     */
	    current_url = globus_libc_strdup(state->dest.url);
	    globus_libc_free(state->dest.url);
	    state->dest.url =globus_libc_strdup(
		globus_gass_transfer_referral_get_url(&referral, 0));

	    globus_gass_transfer_referral_destroy(&referral);

	    if ( (rc = globus_gass_transfer_register_put(
		&request,
		(state->dest.attr->gass_requestattr),
		state->dest.url,
		GLOBUS_NULL,
		globus_l_gass_copy_gass_setup_callback,
		(void *) handle)) != GLOBUS_SUCCESS )
	    { /* there was an error */
		globus_mutex_lock(&state->monitor.mutex);
		err = globus_error_construct_string(
		    GLOBUS_GASS_COPY_MODULE,
		    GLOBUS_NULL,
		    "[%s]: the original destination url: %s was referred to: %s, for which globus_gass_transfer_register_get returned an error code of: %d",
		    myname,
		    current_url,
		    state->dest.url,
		    rc);
		globus_i_gass_copy_set_error(handle, err);
		globus_libc_free(current_url);
		globus_gass_transfer_referral_destroy(&referral);
		goto wakeup_state;
	    }
	    globus_gass_transfer_referral_destroy(&referral);
	}
	globus_libc_free(current_url);
	break;

    case GLOBUS_GASS_TRANSFER_REQUEST_PENDING:
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "request status == GLOBUS_GASS_TRANSFER_REQUEST_PENDING, should signal the monitor\n");
#endif
	if (handle->status == GLOBUS_GASS_COPY_STATUS_INITIAL)
	{
	    globus_mutex_lock(&state->monitor.mutex);
	    state->source.status = GLOBUS_I_GASS_COPY_TARGET_READY;
	    globus_cond_signal(&state->monitor.cond);
	    globus_mutex_unlock(&state->monitor.mutex);
	}
	else
	{
	    globus_mutex_lock(&state->monitor.mutex);
	    state->dest.status = GLOBUS_I_GASS_COPY_TARGET_READY;
	    globus_cond_signal(&state->monitor.cond);
	    globus_mutex_unlock(&state->monitor.mutex);
	}
	break;

    case GLOBUS_GASS_TRANSFER_REQUEST_DONE:
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "request status == GLOBUS_GASS_TRANSFER_REQUEST_DONE\n");
#endif
	globus_mutex_lock(&state->monitor.mutex);
	if (handle->status == GLOBUS_GASS_COPY_STATUS_INITIAL)
	    current_url = state->source.url;
	else
	    current_url = state->dest.url;

	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: we're just getting set up, but the status of url %s is GLOBUS_GASS_TRANSFER_REQUEST_DONE",
	    myname,
	    current_url);
	globus_i_gass_copy_set_error(handle, err);

	goto wakeup_state;
	break;

    case GLOBUS_GASS_TRANSFER_REQUEST_DENIED:
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr,
                "request status == GLOBUS_GASS_TRANSFER_REQUEST_DENIED\n");
#endif
	globus_mutex_lock(&state->monitor.mutex);
	if (handle->status == GLOBUS_GASS_COPY_STATUS_INITIAL)
	    current_url = state->source.url;
	else
	    current_url = state->dest.url;

	denial_reason = globus_gass_transfer_request_get_denial_reason(request);
	denial_message =
            globus_gass_transfer_request_get_denial_message(request);
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]:  url: %s request was DENIED, for reason: %d, %s",
	    myname,
	    current_url,
	    denial_reason,
	    denial_message);
	globus_i_gass_copy_set_error(handle, err);

	goto wakeup_state;
	break;

    case GLOBUS_GASS_TRANSFER_REQUEST_FAILED:
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr,
                "request status == GLOBUS_GASS_TRANSFER_REQUEST_FAILED\n");
#endif
	globus_mutex_lock(&state->monitor.mutex);
	if (handle->status == GLOBUS_GASS_COPY_STATUS_INITIAL)
	    current_url = state->source.url;
	else
	    current_url = state->dest.url;

	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]:  url: %s request FAILED",
	    myname,
	    current_url);
	globus_i_gass_copy_set_error(handle, err);

	goto wakeup_state;
	break;
    } /* switch */
    return;

wakeup_state:
    /*
     * assume mutex has already been locked by above calls
     */
    if (handle->status == GLOBUS_GASS_COPY_STATUS_INITIAL)
	state->source.status = GLOBUS_I_GASS_COPY_TARGET_FAILED;
    else
	state->dest.status = GLOBUS_I_GASS_COPY_TARGET_FAILED;
    handle->status = GLOBUS_GASS_COPY_STATUS_FAILURE;

    globus_gass_transfer_request_destroy(request);
    state->monitor.done = 1;
    globus_cond_signal(&state->monitor.cond);
    globus_mutex_unlock(&state->monitor.mutex);

    return;
} /* globus_l_gass_copy_gass_setup_callback() */

globus_result_t
globus_l_gass_copy_io_setup_get(
    globus_gass_copy_handle_t * handle)
{
    globus_gass_copy_state_t * state = handle->state;
    globus_url_t parsed_url;
    globus_result_t result = GLOBUS_SUCCESS;
    globus_object_t * err;
    static char * myname="globus_l_gass_copy_io_setup_get";

    if (state->source.data.io.free_handle)
    {
	globus_url_parse(state->source.url, &parsed_url);
	state->source.data.io.handle =(globus_io_handle_t *)
	    globus_libc_malloc(sizeof(globus_io_handle_t));

	if(state->source.data.io.handle == GLOBUS_NULL)
	{
	    err = globus_error_construct_string(
		GLOBUS_GASS_COPY_MODULE,
		GLOBUS_NULL,
		"[%s]: failed to malloc a globus_io_handle_t successfully",
		myname);
	    return globus_error_put(err);
	}
	result = globus_io_file_open(
	    parsed_url.url_path,
	    GLOBUS_IO_FILE_RDONLY,
#ifndef TARGET_ARCH_WIN32
		GLOBUS_IO_FILE_IRUSR,
#else
		0,
#endif
	    state->source.attr->io,
	    state->source.data.io.handle);

	if(result==GLOBUS_SUCCESS)
	{
	    state->source.status = GLOBUS_I_GASS_COPY_TARGET_READY;
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	    globus_libc_fprintf(stderr,
                    "io_setup_get(): SUCCESS opening %s\n",parsed_url.url_path);
	}
	else
	{
	    globus_libc_fprintf(stderr,
                    "io_setup_get(): FAILURE opening %s\n",parsed_url.url_path);
#endif
	}
        globus_url_destroy(&parsed_url);
    }
    else
    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "io_setup_get(): handle should already have been  opened by the user\n");
#endif
        state->source.status = GLOBUS_I_GASS_COPY_TARGET_READY;
        result=GLOBUS_SUCCESS;
    }

    return result;

} /* globus_l_gass_copy_io_setup_get() */

globus_result_t
globus_l_gass_copy_io_setup_put(
    globus_gass_copy_handle_t * handle)
{
    globus_gass_copy_state_t * state = handle->state;
    globus_url_t parsed_url;
    globus_result_t result = GLOBUS_SUCCESS;
    globus_object_t * err;
    static char * myname="globus_l_gass_copy_io_setup_put";
    if (state->dest.data.io.free_handle)
    {
        globus_url_parse(state->dest.url, &parsed_url);
        state->dest.data.io.handle = (globus_io_handle_t *)
            globus_libc_malloc(sizeof(globus_io_handle_t));
	if(state->dest.data.io.handle == GLOBUS_NULL)
	{
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	    globus_libc_fprintf(stderr, "io_setup_put(): error mallocing io_handle_t\n");
#endif
	    err = globus_error_construct_string(
		GLOBUS_GASS_COPY_MODULE,
		GLOBUS_NULL,
		"[%s]: failed to malloc a globus_io_handle_t successfully",
		myname);
	    return globus_error_put(err);
	}

        result = globus_io_file_open(
	    parsed_url.url_path,
	    (GLOBUS_IO_FILE_WRONLY|GLOBUS_IO_FILE_CREAT|GLOBUS_IO_FILE_TRUNC),
#ifndef TARGET_ARCH_WIN32
	    (GLOBUS_IO_FILE_IRUSR|GLOBUS_IO_FILE_IWUSR|
	        GLOBUS_IO_FILE_IRGRP|GLOBUS_IO_FILE_IWGRP|
	        GLOBUS_IO_FILE_IROTH|GLOBUS_IO_FILE_IWOTH),
#else
		0,
#endif
	    state->dest.attr->io,
	    state->dest.data.io.handle);

	if(result==GLOBUS_SUCCESS)
	{
	    state->dest.status = GLOBUS_I_GASS_COPY_TARGET_READY;
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	    globus_libc_fprintf(stderr,
                    "io_setup_put(): SUCCESS opening %s\n",parsed_url.url_path);
	}
	else
	{
	    globus_libc_fprintf(stderr,
                    "io_setup_put(): FAILED opening %s\n",parsed_url.url_path);
#endif
	}

        globus_url_destroy(&parsed_url);
    }
    else
    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "io_setup_put(): handle should already have been  opened by the user\n");
#endif
	state->dest.status = GLOBUS_I_GASS_COPY_TARGET_READY;
	result=GLOBUS_SUCCESS;
    }

    return result;
} /* globus_l_gass_copy_io_setup_put() */


globus_result_t
globus_l_gass_copy_ftp_setup_get(
    globus_gass_copy_handle_t * handle)
{
    globus_gass_copy_state_t * state = handle->state;
    globus_result_t result;

    result = globus_ftp_client_get(
		   &(handle->ftp_source_handle),
		   state->source.url,
		   state->source.attr->ftp_attr,
                   GLOBUS_NULL,
		   globus_l_gass_copy_ftp_get_done_callback,
		   (void *) handle);

    if(result==GLOBUS_SUCCESS)
    {
	state->source.status = GLOBUS_I_GASS_COPY_TARGET_READY;
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "ftp_setup_get(): SUCCESS opening %s\n",
            state->source.url);
    }
    else
    {
	globus_libc_fprintf(stderr, "ftp_setup_get(): FAILURE opening %s\n",
            state->source.url);
#endif
    }

    return result;

} /* globus_l_gass_copy_ftp_setup_get() */

globus_result_t
globus_l_gass_copy_ftp_setup_put(
    globus_gass_copy_handle_t * handle)
{
    globus_gass_copy_state_t * state = handle->state;
    globus_result_t result;

    result = globus_ftp_client_put(
		   &(handle->ftp_dest_handle),
		   state->dest.url,
		   state->dest.attr->ftp_attr,
                   GLOBUS_NULL,
		   globus_l_gass_copy_ftp_put_done_callback,
		   (void *) handle);

    if(result==GLOBUS_SUCCESS)
    {
	state->dest.status = GLOBUS_I_GASS_COPY_TARGET_READY;
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr,
                "ftp_setup_put(): SUCCESS opening %s\n",state->dest.url);
    }
    else
    {
        if(handle->performance)
        {
            globus_l_gass_copy_perf_cancel_ftp_callback(handle->performance);
        }

	globus_libc_fprintf(stderr,
                "ftp_setup_put(): FAILURE opening %s\n",state->dest.url);
#endif
    }

    return result;

} /* globus_l_gass_copy_ftp_setup_put() */



void
globus_l_gass_copy_ftp_transfer_callback(
    void *			       user_arg,
    globus_ftp_client_handle_t *       handle,
    globus_object_t *		       error)
{
    globus_object_t * err = GLOBUS_NULL;

    globus_gass_copy_handle_t * copy_handle
	= (globus_gass_copy_handle_t *) user_arg;

#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr, "ftp_transfer_callback(): called\n");
#endif

    if(error != GLOBUS_SUCCESS)
    {
	/* do some error handling */
	/* copy_handle->err = globus_copy_error(error);*/

        if (copy_handle->status != GLOBUS_GASS_COPY_STATUS_CANCEL)
        {
	    globus_i_gass_copy_set_error(copy_handle, error);
	    copy_handle->status = GLOBUS_GASS_COPY_STATUS_FAILURE;
        }

#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "ftp_transfer_callback(): !GLOBUS_SUCESS, error= %d\n",
            error);
#endif
    }
    else
    {
        copy_handle->status = GLOBUS_GASS_COPY_STATUS_DONE;
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "ftp_transfer_callback(): GLOBUS_SUCCESS\n");
#endif
    }

    globus_l_gass_copy_state_free(copy_handle->state);
    copy_handle->state = GLOBUS_NULL;

    if(copy_handle->performance)
    {
        globus_l_gass_copy_perf_cancel_ftp_callback(copy_handle->performance);
    }

#ifdef GLOBUS_I_GASS_COPY_DEBUG
    if(copy_handle->state == GLOBUS_NULL)
	globus_libc_fprintf(stderr, "copy_handle->state == GLOBUS_NULL\n");
    globus_libc_fprintf(stderr, "ftp_transfer_callback(): about to call user callback\n");
#endif

    err = copy_handle->err;
    copy_handle->err = GLOBUS_NULL;

    /* set the final status of the transfer */
    switch(copy_handle->status)
    {
    case GLOBUS_GASS_COPY_STATUS_DONE:
	  copy_handle->status = GLOBUS_GASS_COPY_STATUS_DONE_SUCCESS;
	  break;
    case GLOBUS_GASS_COPY_STATUS_FAILURE:
	  copy_handle->status = GLOBUS_GASS_COPY_STATUS_DONE_FAILURE;
	  break;
    case GLOBUS_GASS_COPY_STATUS_CANCEL:
	  copy_handle->status = GLOBUS_GASS_COPY_STATUS_DONE_CANCELLED;
	  break;
    }


    if(copy_handle->user_callback != GLOBUS_NULL)
	copy_handle->user_callback(
	    copy_handle->callback_arg,
	    copy_handle,
	    err);
    
    if(err)
    {
        globus_object_free(err);
    }
} /* globus_l_gass_copy_ftp_transfer_callback() */

void
globus_l_gass_copy_ftp_get_done_callback(
    void * callback_arg,
    globus_ftp_client_handle_t * handle,
    globus_object_t *	       error)
{
    globus_gass_copy_handle_t * copy_handle
	= (globus_gass_copy_handle_t *) callback_arg;
    globus_i_gass_copy_monitor_t * source_monitor
	= &(copy_handle->state->source.data.ftp.monitor);
    globus_i_gass_copy_cancel_t * cancel_info = GLOBUS_NULL;

#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr, "ftp_get_done_callback(): starting\n");
#endif

    if ((copy_handle->status == GLOBUS_GASS_COPY_STATUS_CANCEL) ||
        (copy_handle->status == GLOBUS_GASS_COPY_STATUS_FAILURE))
    {
        cancel_info = (globus_i_gass_copy_cancel_t *)
            globus_libc_malloc(sizeof(globus_i_gass_copy_cancel_t));
        cancel_info->handle = copy_handle;
        cancel_info->canceling_source = GLOBUS_TRUE;
        globus_mutex_lock(&(source_monitor->mutex));
        source_monitor->done = GLOBUS_TRUE;
        globus_mutex_unlock(&(source_monitor->mutex));

        globus_l_gass_copy_generic_cancel(cancel_info);
        globus_libc_free(cancel_info);
    }
    else
    {
        globus_mutex_lock(&(source_monitor->mutex));
        if (error != GLOBUS_SUCCESS)
        {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	    globus_libc_fprintf(stderr, "ftp_get_done_callback(): called with error\n");
#endif
            globus_i_gass_copy_set_error(copy_handle, error);
        }
        source_monitor->done = GLOBUS_TRUE;
        globus_cond_signal(&(source_monitor->cond));
        globus_mutex_unlock(&(source_monitor->mutex));
    }

} /* globus_l_gass_copy_ftp_get_done_callback() */

void
globus_l_gass_copy_ftp_put_done_callback(
    void * callback_arg,
    globus_ftp_client_handle_t * handle,
    globus_object_t *	       error)
{
    globus_gass_copy_handle_t * copy_handle
	= (globus_gass_copy_handle_t *) callback_arg;
    globus_i_gass_copy_monitor_t * dest_monitor
	= &(copy_handle->state->dest.data.ftp.monitor);
    globus_i_gass_copy_cancel_t * cancel_info = GLOBUS_NULL;

#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr, "ftp_put_done_callback(): starting\n");
#endif

    if ((copy_handle->status == GLOBUS_GASS_COPY_STATUS_CANCEL) ||
        (copy_handle->status == GLOBUS_GASS_COPY_STATUS_FAILURE))
    {
        cancel_info = (globus_i_gass_copy_cancel_t *)
            globus_libc_malloc(sizeof(globus_i_gass_copy_cancel_t));
        cancel_info->handle = copy_handle;
        cancel_info->canceling_source = GLOBUS_FALSE;
        globus_mutex_lock(&(dest_monitor->mutex));
        dest_monitor->done = GLOBUS_TRUE;
        globus_mutex_unlock(&(dest_monitor->mutex));
        globus_l_gass_copy_generic_cancel(cancel_info);
        globus_libc_free(cancel_info);
    }
    else
    {
        globus_mutex_lock(&(dest_monitor->mutex));
        if (error != GLOBUS_SUCCESS)
        {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
            globus_libc_fprintf(stderr, "ftp_put_done_callback(): called with error\n");
#endif
            globus_i_gass_copy_set_error(copy_handle, error);
        }
        dest_monitor->done = GLOBUS_TRUE;
        globus_cond_signal(&(dest_monitor->cond));
        globus_mutex_unlock(&(dest_monitor->mutex));
    }

} /* globus_l_gass_copy_ftp_put_done_callback() */


/*****************************************************************
 * read callbacks
 *****************************************************************/

void
globus_l_gass_copy_generic_read_callback(
    globus_gass_copy_handle_t *    handle,
    globus_byte_t *                 bytes,
    globus_size_t                   nbytes,
    globus_off_t                    offset,
    globus_bool_t                   last_data)
{
    globus_gass_copy_state_t *     state = handle->state;
    globus_i_gass_copy_buffer_t *  buffer_entry;
    globus_object_t *              err;
    globus_bool_t                  push_write = GLOBUS_TRUE;

    static char * myname="globus_l_gass_copy_generic_read_callback";

#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr,
         "generic_read_callback(): read %d bytes, offset: %"GLOBUS_OFF_T_FORMAT", last_data: %d\n",
	  nbytes, offset, last_data);
#endif

    if(state->cancel == GLOBUS_I_GASS_COPY_CANCEL_TRUE)
    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr,
            "generic_read_callback(): there was an error\n");
#endif
	globus_mutex_lock(&(state->source.mutex));
	state->source.n_pending--;
	globus_mutex_unlock(&(state->source.mutex));

        globus_gass_copy_cancel(handle, NULL, NULL);
	return;
    }

    /*
     * - if it's the last_data, set TARGET_DONE to prevent more reads
     * - a buffer entry with last_data = GLOBUS_TRUE is only sent if we
     *      have already received eof and there are no pending callbacks
     * - we only allow zero byte writes if we have already received eof 
     *      and there are no pending callbacks
     */
    globus_mutex_lock(&(state->source.mutex));
    {
        state->source.n_pending--;
        if(last_data && state->source.status == GLOBUS_I_GASS_COPY_TARGET_READY)
        {
            state->source.status = GLOBUS_I_GASS_COPY_TARGET_DONE;
        }
        
        if(state->source.status == GLOBUS_I_GASS_COPY_TARGET_DONE &&
            state->source.n_pending == 0)
        {
            last_data = GLOBUS_TRUE;
        }
        else
        {
            if(nbytes == 0)
            {
                push_write = GLOBUS_FALSE;
            }
            
            last_data = GLOBUS_FALSE;
        }
    }
    globus_mutex_unlock(&(state->source.mutex));
    
    if(push_write)
    {
        /* push the write */
        buffer_entry = (globus_i_gass_copy_buffer_t *)
            globus_libc_malloc(sizeof(globus_i_gass_copy_buffer_t));
        
        if(buffer_entry == GLOBUS_NULL)
        {
            /* out of memory error */
            err = globus_error_construct_string(
        	GLOBUS_GASS_COPY_MODULE,
        	GLOBUS_NULL,
        	"[%s]: failed to malloc a buffer structure successfully",
        	myname);
            globus_i_gass_copy_set_error(handle, err);
        
#ifdef GLOBUS_I_GASS_COPY_DEBUG
            globus_libc_fprintf(stderr,
                    "generic_read_callback(): malloc failed\n");
#endif
            globus_gass_copy_cancel(handle, NULL, NULL);
            return;
        } /* if(buffer_entry == GLOBUS_NULL) */
        
        buffer_entry->bytes  = bytes;
        buffer_entry->nbytes = nbytes;
        buffer_entry->offset = offset;
        buffer_entry->last_data = last_data;
    
        globus_mutex_lock(&(state->dest.mutex));
        {
            /* put this read buffer entry onto the write queue */
            globus_fifo_enqueue( &(state->dest.queue), buffer_entry);
        }
        globus_mutex_unlock(&(state->dest.mutex));
    }
    
    /* start the next write if there isn't already one outstanding */
    if(handle->state)
	globus_l_gass_copy_write_from_queue(handle);
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    else
	globus_libc_fprintf(stderr,
            "generic_read_callback(): handle->state == GLOBUS_NULL\n");
#endif

    /* if we haven't read everything from the source, read again */
    if(handle->state)
	globus_l_gass_copy_read_from_queue(handle);
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    else
	globus_libc_fprintf(stderr,
            "generic_read_callback(): handle->state == GLOBUS_NULL\n");
#endif

} /* globus_l_gass_copy_generic_read_callback() */


void
globus_l_gass_copy_ftp_read_callback(
    void *                          callback_arg,
    globus_ftp_client_handle_t *    handle,
    globus_object_t *               error,
    globus_byte_t *                 bytes,
    globus_size_t                   nbytes,
    globus_off_t                    offset,
    globus_bool_t		    eof)
{
    globus_gass_copy_handle_t * copy_handle
	= (globus_gass_copy_handle_t *) callback_arg;
    globus_gass_copy_state_t * state
        = copy_handle->state;

    globus_bool_t last_data= GLOBUS_FALSE;

#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr,
	"ftp_read_callback(): start, n_pending: %d, nbytes: %d, offset: %"GLOBUS_OFF_T_FORMAT", eof: %d\n",
	state->source.n_pending, nbytes, offset, eof);
#endif

    if(error == GLOBUS_SUCCESS) /* no error occured */
    {
	last_data = eof;
	if(eof)
	{
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	  globus_libc_fprintf(stderr,
	      "ftp_read_callback(): source TARGET_DONE, nbytes: %d, offset: %"GLOBUS_OFF_T_FORMAT", eof: %d\n",
	      nbytes, offset, eof);
#endif
	  /*
	    globus_mutex_lock(&(state->source.mutex));
	    {
		state->source.status = GLOBUS_I_GASS_COPY_TARGET_DONE;
	    }

	    globus_mutex_unlock(&(state->source.mutex));
	  */
	    if((copy_handle->status != GLOBUS_GASS_COPY_STATUS_FAILURE) &&
	       (copy_handle->status < GLOBUS_GASS_COPY_STATUS_READ_COMPLETE))
		copy_handle->status = GLOBUS_GASS_COPY_STATUS_READ_COMPLETE;
	}
    }
    else /* there was an error */
    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "ftp_read_callback: was passed an ERROR\n");
#endif
	{
	    if(!state->cancel) /* cancel has not been set already */
	    {
		globus_i_gass_copy_set_error(copy_handle, error);
		state->cancel = GLOBUS_I_GASS_COPY_CANCEL_TRUE;
		copy_handle->status = GLOBUS_GASS_COPY_STATUS_FAILURE;
	    }
	    else
	    {
	        globus_mutex_lock(&(state->source.mutex));
		state->source.n_pending--;
		globus_mutex_unlock(&(state->source.mutex));
		return;
	    }

	}
    } /* else (there was an error) */


    globus_l_gass_copy_generic_read_callback(
        copy_handle,
        bytes,
        nbytes,
        offset,
	last_data);
} /* globus_l_gass_copy_ftp_read_callback() */

void
globus_l_gass_copy_gass_read_callback(
    void *                          callback_arg,
    globus_gass_transfer_request_t  request,
    globus_byte_t *                 bytes,
    globus_size_t                   nbytes,
    globus_bool_t                   last_data)
{
    globus_off_t offset;
    int req_status;
    globus_object_t * err;
    static char * myname="globus_l_gass_copy_gass_read_callback";

    globus_gass_copy_handle_t * handle
	= (globus_gass_copy_handle_t *) callback_arg;
    globus_gass_copy_state_t * state = handle->state;
    req_status = globus_gass_transfer_request_get_status(request);
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr, "globus_l_gass_copy_gass_read_callback(): req_status= %d\n",
         req_status);
#endif

    if(req_status == GLOBUS_GASS_TRANSFER_REQUEST_DONE ||
       req_status == GLOBUS_GASS_TRANSFER_REQUEST_PENDING)
    { /* all is well */
	if(last_data)
	{ /* this was the last read.  set READ_COMPLETE and free the request */
	  /*
	    globus_mutex_lock(&(state->source.mutex));
	    {
		state->source.status = GLOBUS_I_GASS_COPY_TARGET_DONE;
	    }
	    globus_mutex_unlock(&(state->source.mutex));
	  */
	    handle->status = GLOBUS_GASS_COPY_STATUS_READ_COMPLETE;

         /* req_status = globus_gass_transfer_request_get_status(request); */

	    if(req_status == GLOBUS_GASS_TRANSFER_REQUEST_DONE)
	    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
		globus_libc_fprintf(stderr,
                   "gass_read_callback(): GLOBUS_GASS_TRANSFER_REQUEST_DONE\n");
#endif
		globus_gass_transfer_request_destroy(request);
	    }
	    else
	    {
		/* there's an error, tell someone who cares */
#ifdef GLOBUS_I_GASS_COPY_DEBUG
		globus_libc_fprintf(stderr, "gass_read_callback(): this was last_data, but status !=GLOBUS_GASS_TRANSFER_REQUEST_DONE\n");
#endif
	    }
	}/* if(last_data) */

    } /* all is well */
    else
    { /* all is NOT well, deal with error */
	{
	    if(!state->cancel) /* cancel has not been set already */
	    {
		err = globus_error_construct_string(
		    GLOBUS_GASS_COPY_MODULE,
		    GLOBUS_NULL,
		    "[%s]: gass_transfer_request_status: %d",
		    myname,
		    req_status);
		globus_i_gass_copy_set_error(handle, err);
		state->cancel = GLOBUS_I_GASS_COPY_CANCEL_TRUE;
		handle->status = GLOBUS_GASS_COPY_STATUS_FAILURE;
	    }
	    else
	    {
	        globus_mutex_lock(&(state->source.mutex));
		state->source.n_pending--;
		globus_mutex_unlock(&(state->source.mutex));
		return;
	    }
	}
    } /* else (there was an error) */

    /*offset = state->source.n_complete * handle->buffer_length; */
    offset = state->source.n_complete * handle->buffer_length;
    globus_l_gass_copy_generic_read_callback(
        handle,
        bytes,
        nbytes,
        offset,
	last_data);
    state->source.n_complete++;
} /* globus_l_gass_copy_gass_read_callback() */

void
globus_l_gass_copy_io_read_callback(
    void *                          callback_arg,
    globus_io_handle_t *            io_handle,
    globus_result_t                 result,
    globus_byte_t *                 bytes,
    globus_size_t                   nbytes)
{
    globus_off_t offset;
    globus_object_t * err = GLOBUS_NULL;
    globus_bool_t last_data=GLOBUS_FALSE;
    globus_gass_copy_handle_t * handle
	= (globus_gass_copy_handle_t *) callback_arg;
    globus_gass_copy_state_t * state = handle->state;

#ifdef GLOBUS_I_GASS_COPY_DEBUG
    if(result== GLOBUS_SUCCESS)
	globus_libc_fprintf(stderr,
            "io_read_callback(): result == GLOBUS_SUCCESS\n");
    else
	globus_libc_fprintf(stderr,
            "io_read_callback(): result != GLOBUS_SUCCESS\n");

    globus_libc_fprintf(stderr,
            "io_read_callback(): %d bytes READ\n", nbytes);
#endif

    if(result != GLOBUS_SUCCESS)
    {
	err = globus_error_get(result);
	last_data=globus_io_eof(err);

#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr,
            "io_read_callback(): last_data == %d\n", last_data);
#endif
	if(last_data)
	{ /* this was the last read.  set READ_COMPLETE */
	  /*
	    globus_mutex_lock(&(state->source.mutex));
	    {
		state->source.status = GLOBUS_I_GASS_COPY_TARGET_DONE;
	    }
	    globus_mutex_unlock(&(state->source.mutex));
	  */
	    handle->status = GLOBUS_GASS_COPY_STATUS_READ_COMPLETE;
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	    globus_libc_fprintf(stderr, "io_read_callback(): this was the last READ, source.status == GLOBUS_I_GASS_COPY_TARGET_DONE\n");
#endif
	    if(state->source.data.io.free_handle)
	    {
		globus_io_close(io_handle);
#ifdef GLOBUS_I_GASS_COPY_DEBUG
		globus_libc_fprintf(stderr,
                    "io_read_callback(): handle closed\n");
#endif
		/* thinking that this should go in the
                 * globus_l_gass_copy_state_free()
		 * globus_libc_free(handle);
		 */
	    }
	}/* if(last_data) */
	else  /* there was an error */
	{
	    {
		if(!state->cancel) /* cancel has not been set already */
		{
		    globus_i_gass_copy_set_error(handle, err);
		    state->cancel = GLOBUS_I_GASS_COPY_CANCEL_TRUE;
		    handle->status = GLOBUS_GASS_COPY_STATUS_FAILURE;
		}
		else
		{
	            globus_mutex_lock(&(state->source.mutex));
		    state->source.n_pending--;
		    globus_mutex_unlock(&(state->source.mutex));
		    return;
		}

	    }
	    globus_mutex_unlock(&(state->source.mutex));
	} /* else (there was an error) */
    }

    /* cast to 64 bits if available, otherwise bad things happen */

    offset = ((globus_off_t) state->source.n_complete) *
        ((globus_off_t) handle->buffer_length);
    globus_l_gass_copy_generic_read_callback(
        handle,
        bytes,
        nbytes,
        offset,
	last_data);
    state->source.n_complete++;
} /* globus_l_gass_copy_io_read_callback() */


/*****************************************************************
 * write callbacks
 *****************************************************************/

void
globus_l_gass_copy_generic_write_callback(
    globus_gass_copy_handle_t *    handle,
    globus_byte_t *                bytes,
    globus_size_t                  nbytes,
    globus_off_t                   offset)
{
    globus_gass_copy_state_t * state = handle->state;
    globus_i_gass_copy_buffer_t *  buffer_entry;
    globus_object_t * err;
    static char * myname="globus_l_gass_copy_generic_write_callback";

    globus_mutex_lock(&(state->dest.mutex));
    state->dest.n_pending--;
    globus_mutex_unlock(&(state->dest.mutex));

    if(handle->performance)
    {
        globus_mutex_lock(&handle->performance->lock);
        handle->performance->live_bytes += nbytes;
        globus_mutex_unlock(&handle->performance->lock);
    }

    if(state->cancel == GLOBUS_I_GASS_COPY_CANCEL_TRUE)
    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr,
            "generic_write_callback(): there was an error\n");
#endif
        globus_gass_copy_cancel(handle, NULL, NULL);
	return;
    }


#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr,
        "generic_write_callback(): wrote %d bytes\n", nbytes);
#endif
    /* push the buffer on the read queue and start another read */

    buffer_entry = (globus_i_gass_copy_buffer_t *)
	globus_libc_malloc(sizeof(globus_i_gass_copy_buffer_t));

    if(buffer_entry == GLOBUS_NULL)
    {
	/* out of memory error */
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: failed to malloc a buffer structure successfully",
	    myname);
	globus_i_gass_copy_set_error(handle, err);

#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr,
            "generic_write_callback():  malloc failed\n");
#endif
        globus_gass_copy_cancel(handle, NULL, NULL);
	return;
    }

    buffer_entry->bytes  = bytes;
    globus_mutex_lock(&(state->source.mutex));
    globus_fifo_enqueue( &(state->source.queue), buffer_entry);
    globus_mutex_unlock(&(state->source.mutex));
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr,
        "generic_write_callback(): calling read_from_queue()\n");
#endif
    if(handle->state)
	globus_l_gass_copy_read_from_queue(handle);
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    else
	globus_libc_fprintf(stderr,
            "generic_write_callback(): handle->state == GLOBUS_NULL\n");
#endif

    /* if there are more writes to do, register the next write */
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr,
        "generic_write_callback(): calling write_from_queue()\n");
#endif
    if(handle->state)
	globus_l_gass_copy_write_from_queue(handle);
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    else
	globus_libc_fprintf(stderr,
            "generic_write_callback(): handle->state == GLOBUS_NULL\n");
#endif

} /* globus_l_gass_copy_generic_write_callback() */

void
globus_l_gass_copy_write_from_queue(
    globus_gass_copy_handle_t * handle)
{
    globus_gass_copy_state_t * state = handle->state;
    globus_i_gass_copy_buffer_t *  buffer_entry;
    globus_result_t result = GLOBUS_SUCCESS;
    globus_bool_t do_the_write = GLOBUS_FALSE;
    globus_object_t * err = GLOBUS_NULL;

#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr,
        "globus_l_gass_copy_write_from_queue(): called\n");
#endif

    while(1)
    {
	do_the_write = GLOBUS_FALSE;

	globus_mutex_lock(&(state->dest.mutex));
	{
	    if(state->dest.status == GLOBUS_I_GASS_COPY_TARGET_READY)
	    { /* if the dest is READY (and not DONE), see if we should
               * register a write
               */
#ifdef GLOBUS_I_GASS_COPY_DEBUG
		globus_libc_fprintf(stderr, "write_from_queue(): dest.status == TARGET_READY, n_pending= %d,  n_simultaneous= %d\n", state->dest.n_pending, state->dest.n_simultaneous);
#endif

		if((state->dest.n_pending <
		    state->dest.n_simultaneous) &&
		   !state->cancel)
		{ /* if there aren't too many writes outstanding, and we
                   * haven't canceled
                   */
#ifdef GLOBUS_I_GASS_COPY_DEBUG
		    globus_libc_fprintf(stderr,
                        "write_from_queue: gonna check the queue\n");
#endif
		    if ((buffer_entry=globus_fifo_dequeue(&(state->dest.queue)))
			!= GLOBUS_NULL)
		    {
			state->dest.n_pending++;
			do_the_write = GLOBUS_TRUE;
#ifdef GLOBUS_I_GASS_COPY_DEBUG
			globus_libc_fprintf(stderr,
                            "write_from_queue: got a buffer from the queue\n");
#endif
		    }/* if (buffer_entry != GLOBUS_NULL), there is a buffer
                      * in the write queue
                      */
		    else
		    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
			globus_libc_fprintf(stderr,
                            "write_from_queue: NO buffers in the queue\n");
#endif
		    }
		} /* (n_pending < n_simulatneous) && !cancel */
	    } /* if(state->dest.status == GLOBUS_I_GASS_COPY_TARGET_READY) */
	} /* lock state->dest */
	globus_mutex_unlock(&(state->dest.mutex));
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr,
            "write_from_queue: unlocking the dest mutex\n");
#endif
	if(do_the_write)
	{
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	    globus_libc_fprintf(stderr,
                "write_from_queue(): about to call register_write()\n");
	    globus_libc_fprintf(stderr,
            "\t\t\t nbytes= %d, offset= %"GLOBUS_OFF_T_FORMAT", last_data= %d\n",
                buffer_entry->nbytes,
		buffer_entry->offset,
		buffer_entry->last_data);
#endif
	    result = globus_l_gass_copy_register_write(
		handle,
		buffer_entry);

	    if (result != GLOBUS_SUCCESS)
	    {
		state->cancel = GLOBUS_I_GASS_COPY_CANCEL_TRUE;
#ifdef GLOBUS_I_GASS_COPY_DEBUG
		globus_libc_fprintf(stderr, "write_from_queue(): there was an ERROR trying to register a write, call cancel\n");
#endif
                globus_i_gass_copy_set_error_from_result(handle, result);
                globus_gass_copy_cancel(handle, NULL, NULL);
                return;
	    }
	}  /* if(do_the_write) */
	else
	    break;
    } /* while(1) */

/* if there are no writes to do, and no writes pending, clean up and call
 * user's callback
 */
    if(state->source.status == GLOBUS_I_GASS_COPY_TARGET_DONE &&
       state->dest.status == GLOBUS_I_GASS_COPY_TARGET_DONE )
    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr,
            "write_from_queue(): source and dest status == TARGET_DONE\n");
#endif
	if(state->dest.n_pending == 0 && state->source.n_pending == 0 )
	{ /* our work here is done */
	    handle->status =   GLOBUS_GASS_COPY_STATUS_DONE;

	    /* make sure we get the ftp callbacks, if any */
	    globus_l_gass_copy_wait_for_ftp_callbacks(handle);
	    /* do cleanup */

            if(handle->performance)
            {
                if(state->dest.mode == GLOBUS_GASS_COPY_URL_MODE_FTP)
                {
                    globus_l_gass_copy_perf_cancel_ftp_callback(handle->performance);
                }
                else
                {
                    globus_l_gass_copy_perf_cancel_local_callback(handle->performance);
                }
            }

	    globus_l_gass_copy_state_free(handle->state);
	    handle->state = GLOBUS_NULL;

#ifdef GLOBUS_I_GASS_COPY_DEBUG
	    if(handle->state == GLOBUS_NULL)
		globus_libc_fprintf(stderr, "  handle->state == GLOBUS_NULL\n");
	    globus_libc_fprintf(stderr,
                "write_from_queue(): about to call user callback\n");
#endif
	    err = handle->err;
	    handle->err = GLOBUS_NULL;

	    /* set the final status of the transfer */
	    switch(handle->status)
	    {
	    case GLOBUS_GASS_COPY_STATUS_DONE:
	      handle->status = GLOBUS_GASS_COPY_STATUS_DONE_SUCCESS;
	      break;
	    case GLOBUS_GASS_COPY_STATUS_FAILURE:
	      handle->status = GLOBUS_GASS_COPY_STATUS_DONE_FAILURE;
	      break;
	    case GLOBUS_GASS_COPY_STATUS_CANCEL:
	      handle->status = GLOBUS_GASS_COPY_STATUS_DONE_CANCELLED;
	      break;
	    }

	    if(handle->user_callback != GLOBUS_NULL)
		handle->user_callback(
		    handle->callback_arg,
		    handle,
		    err);
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	    globus_libc_fprintf(stderr,
                "write_from_queue(): done calling user callback\n");
#endif
	    /* if an error object was created, free it */
	    if(err != GLOBUS_NULL)
		globus_object_free(err);
	} /*  if(state->dest.n_pending == 0 && state->source.n_pending == 0 )*/
    } /* if both source and dest are GLOBUS_I_GASS_COPY_TARGET_DONE */
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr, "write_from_queue(): returning\n");
#endif
} /* globus_l_gass_copy_write_from_queue() */

globus_result_t
globus_l_gass_copy_register_write(
    globus_gass_copy_handle_t * handle,
    globus_i_gass_copy_buffer_t * buffer_entry)
{
    globus_result_t result =GLOBUS_SUCCESS;
    globus_gass_copy_state_t * state = handle->state;
    int rc;
    globus_object_t * err;
    static char * myname="globus_l_gass_copy_register_write";

    switch (state->dest.mode)
    {
    case GLOBUS_GASS_COPY_URL_MODE_FTP:
	/* check the offset to see if its what we are expecting */
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr,
            "register_write():  calling globus_ftp_client_register_write()\n");
	globus_libc_fprintf(stderr,
            "\t\t\t nbytes= %d, offset= %"GLOBUS_OFF_T_FORMAT", last_data= %d\n",
                buffer_entry->nbytes,
		buffer_entry->offset,
		buffer_entry->last_data);
#endif
	result = globus_ftp_client_register_write(
	    &(handle->ftp_dest_handle),
	    buffer_entry->bytes,
	    buffer_entry->nbytes,
	    buffer_entry->offset,
	    buffer_entry->last_data,
	    globus_l_gass_copy_ftp_write_callback,
	    (void *) handle);

	break;

    case GLOBUS_GASS_COPY_URL_MODE_GASS:
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr,
             "register_write(): send_bytes -- %d bytes (last_data==%d)\n",
             buffer_entry->nbytes,
             buffer_entry->last_data);
#endif
	/* check the offset to see if its what we are expecting */
	rc = globus_gass_transfer_send_bytes(
	    state->dest.data.gass.request,
	    buffer_entry->bytes,
	    buffer_entry->nbytes,
	    buffer_entry->last_data,
	    globus_l_gass_copy_gass_write_callback,
	    (void *) handle);

	if (rc != GLOBUS_SUCCESS)
	{
	    /* figure out what the error is, and pass it back through the
             * result
             */
	    err = globus_error_construct_string(
		GLOBUS_GASS_COPY_MODULE,
		GLOBUS_NULL,
		"[%s]: globus_gass_transfer_send_bytes returned error code: %d",
		myname,
		rc);
	    globus_i_gass_copy_set_error(handle, err);
	    result = globus_error_put(err);
	}
	else result = GLOBUS_SUCCESS;

	break;

    case GLOBUS_GASS_COPY_URL_MODE_IO:

	if (state->dest.data.io.seekable &&
	    state->source.mode == GLOBUS_GASS_COPY_URL_MODE_FTP)
	{
	    result = globus_io_file_seek(
		state->dest.data.io.handle,
		buffer_entry->offset,
		GLOBUS_IO_SEEK_SET);
	}

	if(result == GLOBUS_SUCCESS)
	{
	    result = globus_io_register_write(
		state->dest.data.io.handle,
		buffer_entry->bytes,
		buffer_entry->nbytes,
		globus_l_gass_copy_io_write_callback,
		(void *) handle);
	}

	break;
    }/* switch (state->dest.mode) */

    globus_libc_free(buffer_entry);

    return result;
}/* globus_l_gass_copy_register_write() */

void
globus_l_gass_copy_ftp_write_callback(
    void *                       callback_arg,
    globus_ftp_client_handle_t * handle,
    globus_object_t *            error,
    globus_byte_t *              bytes,
    globus_size_t                nbytes,
    globus_off_t                 offset,
    globus_bool_t		 eof)
{
    globus_gass_copy_handle_t * copy_handle
	= (globus_gass_copy_handle_t *) callback_arg;
    globus_gass_copy_state_t * state
        = copy_handle->state;

#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr,
        "ftp_write_callback():  has been called, nbytes: %d, offset= %"GLOBUS_OFF_T_FORMAT", eof= %d\n",
	nbytes, offset, eof);
#endif

    if(error == GLOBUS_SUCCESS) /* no error occured */
    {
	if(eof)
	{
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	    globus_libc_fprintf(stderr,
	        "ftp_write_callback(): about to set dest TARGET_DONE, nbytes: %d, offset= %"GLOBUS_OFF_T_FORMAT", eof= %d\n",
	nbytes, offset, eof);
#endif
	    globus_mutex_lock(&(state->dest.mutex));
	    {
		state->dest.status = GLOBUS_I_GASS_COPY_TARGET_DONE;
	    }
	    globus_mutex_unlock(&(state->dest.mutex));
	    if((copy_handle->status != GLOBUS_GASS_COPY_STATUS_FAILURE) &&
	       (copy_handle->status < GLOBUS_GASS_COPY_STATUS_WRITE_COMPLETE))
		copy_handle->status = GLOBUS_GASS_COPY_STATUS_WRITE_COMPLETE;
	}
    }
    else /* there was an error */
    {
	{
	    if(!state->cancel) /* cancel has not been set already */
	    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
		globus_libc_fprintf(stderr, "ftp_write_callback(): there was an ERROR, throw cancel flag\n");
#endif
		globus_i_gass_copy_set_error(copy_handle, error);
		state->cancel = GLOBUS_I_GASS_COPY_CANCEL_TRUE;
		copy_handle->status = GLOBUS_GASS_COPY_STATUS_FAILURE;
	    }
	    else
	    {
	        globus_mutex_lock(&(state->dest.mutex));
		state->dest.n_pending--;
		globus_mutex_unlock(&(state->dest.mutex));
		return;
	    }

	}
    } /* else (there was an error) */

    globus_l_gass_copy_generic_write_callback(
        copy_handle,
        bytes,
        nbytes,
        offset);
} /* globus_l_gass_copy_ftp_write_callback() */


void
globus_l_gass_copy_gass_write_callback(
    void *                          callback_arg,
    globus_gass_transfer_request_t  request,
    globus_byte_t *                 bytes,
    globus_size_t                   nbytes,
    globus_bool_t                   last_data)
{
    int req_status;
    globus_object_t * err;
    static char * myname="globus_l_gass_copy_gass_write_callback";

    globus_gass_copy_handle_t * handle
	= (globus_gass_copy_handle_t *) callback_arg;
    globus_gass_copy_state_t * state = handle->state;

    req_status = globus_gass_transfer_request_get_status(request);
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr,
        "gass_write_callback(): last_data== %d, req_status= %d\n",
        last_data, req_status);
#endif

    if(req_status == GLOBUS_GASS_TRANSFER_REQUEST_DONE ||
       req_status == GLOBUS_GASS_TRANSFER_REQUEST_PENDING)
    { /* all is well */
	if(last_data)
	{ /* this was the last write. set WRITE_COMPLETE and free the request */
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	    globus_libc_fprintf(stderr,
                "gass_write_callback(): THIS WAS THE LAST WRITE\n");
#endif
	    globus_mutex_lock(&(state->dest.mutex));
	    {
		state->dest.status = GLOBUS_I_GASS_COPY_TARGET_DONE;
	    }
	    globus_mutex_unlock(&(state->dest.mutex));
	    handle->status = GLOBUS_GASS_COPY_STATUS_WRITE_COMPLETE;

	    if(req_status == GLOBUS_GASS_TRANSFER_REQUEST_DONE)
	    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
		globus_libc_fprintf(stderr,
                  "gass_write_callback(): GLOBUS_GASS_TRANSFER_REQUEST_DONE\n");
#endif
		globus_gass_transfer_request_destroy(request);
	    }
	    else
	    {
		/* there's an error, tell someone who cares */
#ifdef GLOBUS_I_GASS_COPY_DEBUG
		globus_libc_fprintf(stderr, "gass_write_callback(): this was last_data, but status !=GLOBUS_GASS_TRANSFER_REQUEST_DONE\n");
#endif
	    }
	} /* if (last_data) */
    } /*all is well */
    else
    { /* all is NOT well, deal with error */
	{
	    if(!state->cancel) /* cancel has not been set already */
	    {
		err = globus_error_construct_string(
		    GLOBUS_GASS_COPY_MODULE,
		    GLOBUS_NULL,
		    "[%s]: gass_transfer_request_status: %d",
		    myname,
		    req_status);
		globus_i_gass_copy_set_error(handle, err);
		state->cancel = GLOBUS_I_GASS_COPY_CANCEL_TRUE;
		handle->status = GLOBUS_GASS_COPY_STATUS_FAILURE;
	    }
	    else
	    {
	        globus_mutex_lock(&(state->dest.mutex));
		state->dest.n_pending--;
		globus_mutex_unlock(&(state->dest.mutex));
		return;
	    }
	}
    } /* else (there was an error) */

    globus_l_gass_copy_generic_write_callback(
        handle,
        bytes,
        nbytes,
        0);
} /* globus_l_gass_copy_gass_write_callback() */

void
globus_l_gass_copy_io_write_callback(
    void *                callback_arg,
    globus_io_handle_t *  io_handle,
    globus_result_t       result,
    globus_byte_t *       bytes,
    globus_size_t         nbytes)
{
    globus_object_t * err;

    globus_gass_copy_handle_t * handle
	= (globus_gass_copy_handle_t *) callback_arg;
    globus_gass_copy_state_t * state = handle->state;

/**
 * used this to simulate a io write error
 *
    err = globus_error_construct_string(
	GLOBUS_GASS_COPY_MODULE,
	GLOBUS_NULL,
	"[STU]: STU, forcing io write callback fault");
    result=globus_error_put(err);
*/

    if(result==GLOBUS_SUCCESS)
    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr,
            "io_write_callback(): result == GLOBUS_SUCCESS\n");
#endif

	globus_mutex_lock(&(state->source.mutex));
	{
	    if(state->source.status == GLOBUS_I_GASS_COPY_TARGET_DONE &&
	       state->source.n_pending == 0)
	    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
		globus_libc_fprintf(stderr,
                    "io_write_callback(): THIS WAS THE LAST WRITE\n");
#endif
		globus_mutex_lock(&(state->dest.mutex));
		{
		    if(globus_fifo_empty(&(state->dest.queue)))
		    {
			state->dest.status = GLOBUS_I_GASS_COPY_TARGET_DONE;

			handle->status = GLOBUS_GASS_COPY_STATUS_WRITE_COMPLETE;

			if(state->dest.data.io.free_handle)
			{
			    globus_io_close(io_handle);
#ifdef GLOBUS_I_GASS_COPY_DEBUG
			    globus_libc_fprintf(stderr,
                                "io_write_callback(): handle closed\n");
#endif
			} /* if(state->dest.data.io.free_handle) */
		    } /* if write queue is empty */
		}
		globus_mutex_unlock(&(state->dest.mutex));

	    } /* end if last write */
	}
	globus_mutex_unlock(&(state->source.mutex));
    }
    else /* there was an error */
    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr,
            "io_write_callback(): result != GLOBUS_SUCCESS\n");
#endif
	{
	    if(!state->cancel) /* cancel has not been set already */
	    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	        globus_libc_fprintf(stderr,
                    "io_write_callback(): cancel has not been set\n");
#endif
		globus_i_gass_copy_set_error_from_result(handle, result);
		state->cancel = GLOBUS_I_GASS_COPY_CANCEL_TRUE;
		handle->status = GLOBUS_GASS_COPY_STATUS_FAILURE;
	    }
	    else
	    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	        globus_libc_fprintf(stderr,
                    "io_write_callback(): cancel has already been set\n");
#endif
	        globus_mutex_lock(&(state->dest.mutex));
		state->dest.n_pending--;
		globus_mutex_unlock(&(state->dest.mutex));
		return;
	    }
	}
    } /* else (there was an error) */

    globus_l_gass_copy_generic_write_callback(
        handle,
        bytes,
        nbytes,
        0);
} /* globus_l_gass_copy_io_write_callback() */

#endif /* GLOBUS_DONT_DOCUMENT_INTERNAL */

/************************************************************
 * Transfer functions (synchronous)
 ************************************************************/

/**
 * Transfer data from source URL to destination URL (blocking)
 *
 * @param handle
 *        The handle to perform the copy operation
 * @param source_url
 *        transfer data from this URL
 * @param source_attr
 *        Attributes describing how the transfer form the source should be done
 * @param dest_url
 *        transfer data to this URL
 * @param dest_attr
 *        Attributes describing how the transfer to the destination should be
 *        done
 *
 * @return
 *         This function returns GLOBUS_SUCCESS if the transfer was completed
 *         successfully, or a result pointing to an
 *         object of one of the the following error types:
 * @retval GLOBUS_GASS_COPY_ERROR_TYPE_NULL_PARAMETER
 *         The handle was equal to GLOBUS_NULL, so the transfer could not
 *         processed.
 * @retval GLOBUS_GASS_COPY_ERROR_TYPE_next_error
 *         next error description
 *
 * @see globus_gass_copy_url_to_handle() globus_gass_copy_handle_to_url()
 */
globus_result_t
globus_gass_copy_url_to_url(
    globus_gass_copy_handle_t * handle,
    char * source_url,
    globus_gass_copy_attr_t * source_attr,
    char * dest_url,
    globus_gass_copy_attr_t * dest_attr)
{
    globus_result_t result;
    globus_i_gass_copy_monitor_t        monitor;
    globus_object_t * err;
    int bad_param;
    static char * myname="globus_gass_copy_url_to_url";

    if(handle == GLOBUS_NULL)
    {
	bad_param = 1;
	goto error_exit;
    }
    if(source_url == GLOBUS_NULL)
    {
	bad_param = 2;
	goto error_exit;
    }
    if(dest_url == GLOBUS_NULL)
    {
	bad_param = 4;
	goto error_exit;
    }

     /* return an error if a transfer is already in progress */
    if(handle->status > GLOBUS_GASS_COPY_STATUS_NONE &&
       handle->status < GLOBUS_GASS_COPY_STATUS_DONE_SUCCESS)
    {
      err = globus_error_construct_string(
	GLOBUS_GASS_COPY_MODULE,
	GLOBUS_NULL,
	"[%s]: There is a transfer already active on this handle",
	myname);
      return globus_error_put(err);
    }

    globus_mutex_init(&monitor.mutex, GLOBUS_NULL);
    globus_cond_init(&monitor.cond, GLOBUS_NULL);
    monitor.done = GLOBUS_FALSE;
    monitor.err = GLOBUS_NULL;
    monitor.use_err = GLOBUS_FALSE;

    result = globus_gass_copy_register_url_to_url(
	handle,
	source_url,
	source_attr,
	dest_url,
	dest_attr,
	globus_l_gass_copy_monitor_callback,
	(void *) &monitor);

    if(result != GLOBUS_SUCCESS)
    {
	globus_mutex_destroy(&monitor.mutex);
	globus_cond_destroy(&monitor.cond);
	handle->status = GLOBUS_GASS_COPY_STATUS_DONE_FAILURE;
	return(result);
    }
    /* wait on cond_wait() for completion */
    globus_mutex_lock(&monitor.mutex);

    while(!monitor.done)
    {
        globus_cond_wait(&monitor.cond, &monitor.mutex);
    }

    globus_mutex_unlock(&monitor.mutex);

    globus_mutex_destroy(&monitor.mutex);
    globus_cond_destroy(&monitor.cond);
    /* do some error checking
     */
    if(monitor.use_err)
    {
        return globus_error_put(monitor.err);
    }
    else
    {
        return GLOBUS_SUCCESS;
    }

error_exit:

    err = globus_error_construct_string(
	GLOBUS_GASS_COPY_MODULE,
	GLOBUS_NULL,
	"[%s]: BAD_PARAMETER, argument %d cannot be NULL",
	myname,
	bad_param);

    if(handle)
	handle->status = GLOBUS_GASS_COPY_STATUS_DONE_FAILURE;

    return globus_error_put(err);

} /* globus_gass_copy_url_to_url() */



/*****************************************************************
 * copy url to handle
 *****************************************************************/

/**
 * Transfer data from source URL to an IO handle (blocking)
 *
 * @param handle
 *        The handle to perform the copy operation
 * @param source_url
 *        transfer data from this URL
 * @param source_attr
 *        Attributes describing how the transfer form the source should be done
 * @param dest_handle
 *        transfer data to this IO handle
 *
 *
 * @return
 *         This function returns GLOBUS_SUCCESS if the transfer was completed
 *         successfully, or a result pointing to an
 *         object of one of the the following error types:
 * @retval GLOBUS_GASS_COPY_ERROR_TYPE_NULL_PARAMETER
 *         The handle was equal to GLOBUS_NULL, so the transfer could not
 *         processed.
 * @retval GLOBUS_GASS_COPY_ERROR_TYPE_next_error
 *         next error description
 *
 * @see globus_gass_copy_url_to_url() globus_gass_copy_handle_to_url()
 */
globus_result_t
globus_gass_copy_url_to_handle(
    globus_gass_copy_handle_t * handle,
    char * source_url,
    globus_gass_copy_attr_t * source_attr,
    globus_io_handle_t * dest_handle)
{
    globus_result_t result;
    globus_i_gass_copy_monitor_t        monitor;
    globus_object_t * err;
    int bad_param;
    static char * myname="globus_gass_copy_url_to_handle";

    /* Check arguments for validity */
    if(handle == GLOBUS_NULL)
    {
	bad_param=1;
	goto error_exit;
    }
    if(source_url == GLOBUS_NULL)
    {
	bad_param=2;
	goto error_exit;
    }
    if(dest_handle == GLOBUS_NULL)
    {
	bad_param=4;
	goto error_exit;
    }

    /* return an error if a transfer is already in progress */
    if(handle->status > GLOBUS_GASS_COPY_STATUS_NONE &&
       handle->status < GLOBUS_GASS_COPY_STATUS_DONE_SUCCESS)
    {
      err = globus_error_construct_string(
	GLOBUS_GASS_COPY_MODULE,
	GLOBUS_NULL,
	"[%s]: There is a transfer already active on this handle",
	myname);
      return globus_error_put(err);
    }

    globus_mutex_init(&monitor.mutex, GLOBUS_NULL);
    globus_cond_init(&monitor.cond, GLOBUS_NULL);
    monitor.done = GLOBUS_FALSE;
    monitor.err = GLOBUS_NULL;
    monitor.use_err = GLOBUS_FALSE;

    result = globus_gass_copy_register_url_to_handle(
	handle,
	source_url,
	source_attr,
	dest_handle,
	globus_l_gass_copy_monitor_callback,
	(void *) &monitor);

    if(result != GLOBUS_SUCCESS)
    {
	globus_mutex_destroy(&monitor.mutex);
	globus_cond_destroy(&monitor.cond);
	handle->status = GLOBUS_GASS_COPY_STATUS_DONE_FAILURE;
	return(result);
    }

    /* wait on cond_wait() for completion */
    globus_mutex_lock(&monitor.mutex);

    while(!monitor.done)
    {
	globus_cond_wait(&monitor.cond, &monitor.mutex);
    }

    globus_mutex_unlock(&monitor.mutex);

    globus_mutex_destroy(&monitor.mutex);
    globus_cond_destroy(&monitor.cond);
    /* do some error checking
     */

    if(monitor.use_err)
    {
        return globus_error_put(monitor.err);
    }
    else
    {
        return GLOBUS_SUCCESS;
    }

error_exit:

    err = globus_error_construct_string(
	GLOBUS_GASS_COPY_MODULE,
	GLOBUS_NULL,
	"[%s]: BAD_PARAMETER, argument %d cannot be NULL",
	myname,
	bad_param);

    if(handle)
	handle->status = GLOBUS_GASS_COPY_STATUS_DONE_FAILURE;
    return globus_error_put(err);

} /* globus_gass_copy_url_to_handle() */


/**
 * Transfer data from an IO handle to destination URL  (blocking)
 *
 * @param handle
 *        The handle to perform the copy operation
 * @param source_handle
 *        transfer data from this IO handle
 * @param dest_url
 *        transfer data to this URL
 * @param dest_attr
 *        Attributes describing how the transfer to the destination should be
 *        done
 *
 *
 * @return
 *         This function returns GLOBUS_SUCCESS if the transfer was completed
 *         successfully, or a result pointing to an
 *         object of one of the the following error types:
 * @retval GLOBUS_GASS_COPY_ERROR_TYPE_NULL_PARAMETER
 *         The handle was equal to GLOBUS_NULL, so the transfer could not
 *         processed.
 * @retval GLOBUS_GASS_COPY_ERROR_TYPE_next_error
 *         next error description
 *
 * @see globus_gass_copy_url_to_url() globus_gass_copy_url_to_handle()
 */

globus_result_t
globus_gass_copy_handle_to_url(
    globus_gass_copy_handle_t * handle,
    globus_io_handle_t * source_handle,
    char * dest_url,
    globus_gass_copy_attr_t * dest_attr)
{
    globus_result_t result;
    globus_i_gass_copy_monitor_t        monitor;
    globus_object_t * err;
    int bad_param;
    static char * myname="globus_gass_copy_handle_to_url";

    /* Check arguments for validity */
    if(handle == GLOBUS_NULL)
    {
	bad_param=1;
	goto error_exit;
    }
    if(source_handle == GLOBUS_NULL)
    {
	bad_param=2;
	goto error_exit;
    }
    if(dest_url == GLOBUS_NULL)
    {
	bad_param=3;
	goto error_exit;
    }

    /* return an error if a transfer is already in progress */
    if(handle->status > GLOBUS_GASS_COPY_STATUS_NONE &&
       handle->status < GLOBUS_GASS_COPY_STATUS_DONE_SUCCESS)
    {
      err = globus_error_construct_string(
	GLOBUS_GASS_COPY_MODULE,
	GLOBUS_NULL,
	"[%s]: There is a transfer already active on this handle",
	myname);
      return globus_error_put(err);
    }

    globus_mutex_init(&monitor.mutex, GLOBUS_NULL);
    globus_cond_init(&monitor.cond, GLOBUS_NULL);
    monitor.done = GLOBUS_FALSE;
    monitor.err = GLOBUS_NULL;
    monitor.use_err = GLOBUS_FALSE;

    result = globus_gass_copy_register_handle_to_url(
	handle,
	source_handle,
	dest_url,
	dest_attr,
	globus_l_gass_copy_monitor_callback,
	(void *) &monitor);

    if(result != GLOBUS_SUCCESS)
    {
	globus_mutex_destroy(&monitor.mutex);
	globus_cond_destroy(&monitor.cond);
	handle->status = GLOBUS_GASS_COPY_STATUS_DONE_FAILURE;
	return(result);
    }
    /* wait on cond_wait() for completion */
    globus_mutex_lock(&monitor.mutex);

    while(!monitor.done)
    {
	globus_cond_wait(&monitor.cond, &monitor.mutex);
    }

    globus_mutex_unlock(&monitor.mutex);

    globus_mutex_destroy(&monitor.mutex);
    globus_cond_destroy(&monitor.cond);
    /* do some error checking
     */

    if(monitor.use_err)
    {
	return globus_error_put(monitor.err);
    }
    else
    {
	return GLOBUS_SUCCESS;
    }

error_exit:

    err = globus_error_construct_string(
	GLOBUS_GASS_COPY_MODULE,
	GLOBUS_NULL,
	"[%s]: BAD_PARAMETER, argument %d cannot be NULL",
	myname,
	bad_param);

    if(handle)
	handle->status = GLOBUS_GASS_COPY_STATUS_DONE_FAILURE;

    return globus_error_put(err);

} /* globus_gass_copy_handle_to_url() */

/************************************************************
 * Transfer functions (asynchronous)
 ************************************************************/

/**
 * Transfer data from source URL to destination URL (non-blocking)
 *
 * This functions initiates a transfer from source URL to destination URL,
 * then returns immediately.
 *
 * When the transfer is completed or if the
 * transfer is aborted, the callback_func will be invoked with the final
 * status of the transfer.
 *
 * @param handle
 *        The handle to perform the copy operation
 * @param source_url
 *        transfer data from this URL
 * @param source_attr
 *        Attributes describing how the transfer form the source should be done
 * @param dest_url
 *        transfer data to this URL
 * @param dest_attr
 *        Attributes describing how the transfer to the destination should be
 *        done
 * @param callback_func
 *        Callback to be invoked once the transfer is completed.
 * @param callback_arg
 *        Argument to be passed to the callback_func.
 *
 * @return
 *         This function returns GLOBUS_SUCCESS if the transfer was initiated
 *         successfully, or a result pointing to an
 *         object of one of the the following error types:
 * @retval GLOBUS_GASS_COPY_ERROR_TYPE_NULL_PARAMETER
 *         The handle was equal to GLOBUS_NULL, so the transfer could not
 *         processed.
 * @retval GLOBUS_GASS_COPY_ERROR_TYPE_next_error
 *         next error description
 *
 * @see globus_gass_copy_register_url_to_handle(),
 *      globus_gass_copy_register_handle_to_url()
 */
globus_result_t
globus_gass_copy_register_url_to_url(
    globus_gass_copy_handle_t * handle,
    char * source_url,
    globus_gass_copy_attr_t * source_attr,
    char * dest_url,
    globus_gass_copy_attr_t * dest_attr,
    globus_gass_copy_callback_t callback_func,
    void * callback_arg)
{
    globus_object_t * err = GLOBUS_ERROR_NO_INFO;
    globus_result_t result;
    globus_gass_copy_state_t * state;
    globus_gass_copy_url_mode_t source_url_mode;
    globus_gass_copy_url_mode_t dest_url_mode;
    int bad_param;
    static char * myname="globus_gass_copy_register_url_to_url";

    /* Check arguments for validity */
    if(handle == GLOBUS_NULL)
    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr,
            "register_url_to_url(): handle was GLOBUS_NULL\n");
#endif
	bad_param = 1;
	goto error_exit;
    }
    if(source_url == GLOBUS_NULL)
    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr,
            "register_url_to_url(): source_url  was GLOBUS_NULL\n");
#endif
	bad_param = 2;
	goto error_exit;
    }
    if(dest_url == GLOBUS_NULL)
    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr,
            "register_url_to_url(): dest_url was GLOBUS_NULL\n");
#endif
	bad_param = 4;
	goto error_exit;
    }

    /* return an error if a transfer is already in progress */
    if(handle->status > GLOBUS_GASS_COPY_STATUS_NONE &&
       handle->status < GLOBUS_GASS_COPY_STATUS_DONE_SUCCESS)
    {
      err = globus_error_construct_string(
	GLOBUS_GASS_COPY_MODULE,
	GLOBUS_NULL,
	"[%s]: There is a transfer already active on this handle",
	myname);
      return globus_error_put(err);
    }

    result = globus_gass_copy_get_url_mode(
	source_url,
	&source_url_mode);
    if(result != GLOBUS_SUCCESS)
    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "register_url_to_url(): copy_url_mode returned ! GLOBUS_SUCCESS for source_url\n");
#endif
	goto error_result_exit;
    }

    result = globus_gass_copy_get_url_mode(
	dest_url,
	&dest_url_mode);
    if(result != GLOBUS_SUCCESS)
    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "register_url_to_url(): copy_url_mode returned ! GLOBUS_SUCCESS for dest_url\n");
#endif
	goto error_result_exit;
    }

    if (   (source_url_mode == GLOBUS_GASS_COPY_URL_MODE_UNSUPPORTED)
	   || (dest_url_mode == GLOBUS_GASS_COPY_URL_MODE_UNSUPPORTED) )
    {
	char src_msg[256];
	char dest_msg[256];
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr,
            "register_url_to_url(): source or dest is URL_MODE_UNSUPPORTED\n");
#endif
	if(source_url_mode == GLOBUS_GASS_COPY_URL_MODE_UNSUPPORTED)
	{
	    sprintf(src_msg, "  %s,  GLOBUS_GASS_COPY_URL_MODE_UNSUPPORTED.",
                source_url);
        }
        else
        {
            *src_msg = '\0';
        }

	if(dest_url_mode == GLOBUS_GASS_COPY_URL_MODE_UNSUPPORTED)
	{
	    sprintf(dest_msg, "  %s,  GLOBUS_GASS_COPY_URL_MODE_UNSUPPORTED.",
                dest_url);
        }
        else
        {
            *dest_msg = '\0';
        }

	handle->status = GLOBUS_GASS_COPY_STATUS_DONE_FAILURE;
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: %s%s",
	    myname,
	    src_msg,
	    dest_msg);

	return globus_error_put(err);
    }

    /* Initialize the state for this transfer */
    result = globus_l_gass_copy_state_new(handle);
    if(result != GLOBUS_SUCCESS) goto error_result_exit;

    state = handle->state;
    state->cancel = GLOBUS_I_GASS_COPY_CANCEL_FALSE;
    /*store the user's callback and argument */
    handle->user_callback = callback_func;
    handle->callback_arg = callback_arg;

    result = globus_l_gass_copy_target_populate(
	handle,
	&(state->source),
	&source_url_mode,
	source_url,
	source_attr);

    if(result != GLOBUS_SUCCESS) goto error_result_exit;
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr, "source target populated\n");
#endif
    result = globus_l_gass_copy_target_populate(
	handle,
	&(state->dest),
	&dest_url_mode,
	dest_url,
	dest_attr);

    if(result != GLOBUS_SUCCESS) goto error_result_exit;
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr, "dest target populated\n");
#endif

    if (   (source_url_mode == GLOBUS_GASS_COPY_URL_MODE_FTP) &&
	   (dest_url_mode == GLOBUS_GASS_COPY_URL_MODE_FTP) &&
	   !handle->no_third_party_transfers )
    {

#ifdef GLOBUS_I_GASS_COPY_DEBUG
        globus_libc_fprintf(stderr,
            "calling globus_ftp_client_third_party_transfer()\n");
#endif

        if(handle->performance)
        {
            globus_ftp_client_operationattr_t * new_ftp_attr;

            new_ftp_attr = GLOBUS_NULL;

            /* to get perf markers in 3pt we MUST have EB mode enabled */
            if(state->dest.attr->ftp_attr)
            {
                globus_ftp_control_mode_t   mode;
                result = globus_ftp_client_operationattr_get_mode(
                    state->dest.attr->ftp_attr,
                    &mode);

                if(result != GLOBUS_SUCCESS ||
                    mode != GLOBUS_FTP_CONTROL_MODE_EXTENDED_BLOCK)
                {
                    new_ftp_attr = (globus_ftp_client_operationattr_t *)
		        globus_libc_malloc(sizeof(globus_ftp_client_operationattr_t));

		    globus_ftp_client_operationattr_copy(new_ftp_attr,
		        state->dest.attr->ftp_attr);
                }
            }
            else
            {
                new_ftp_attr = (globus_ftp_client_operationattr_t *)
		        globus_libc_malloc(sizeof(globus_ftp_client_operationattr_t));
                globus_ftp_client_operationattr_init(new_ftp_attr);
            }

            if(new_ftp_attr)
            {
                handle->performance->saved_dest_attr = GLOBUS_TRUE;
                handle->performance->dest_ftp_attr = state->dest.attr->ftp_attr;

                globus_ftp_client_operationattr_set_mode(new_ftp_attr,
                    GLOBUS_FTP_CONTROL_MODE_EXTENDED_BLOCK);

                state->dest.attr->ftp_attr = new_ftp_attr;
            }

            new_ftp_attr = GLOBUS_NULL;

            if(state->source.attr->ftp_attr)
            {
                globus_ftp_control_mode_t   mode;
                result = globus_ftp_client_operationattr_get_mode(
                    state->source.attr->ftp_attr,
                    &mode);

                if(result != GLOBUS_SUCCESS ||
                    mode != GLOBUS_FTP_CONTROL_MODE_EXTENDED_BLOCK)
                {
                    new_ftp_attr = (globus_ftp_client_operationattr_t *)
		        globus_libc_malloc(sizeof(globus_ftp_client_operationattr_t));

		    globus_ftp_client_operationattr_copy(new_ftp_attr,
		        state->source.attr->ftp_attr);
                }
            }
            else
            {
                new_ftp_attr = (globus_ftp_client_operationattr_t *)
		        globus_libc_malloc(sizeof(globus_ftp_client_operationattr_t));
                globus_ftp_client_operationattr_init(new_ftp_attr);
            }

            if(new_ftp_attr)
            {
                handle->performance->saved_source_attr = GLOBUS_TRUE;
                handle->performance->source_ftp_attr = state->source.attr->ftp_attr;

                globus_ftp_client_operationattr_set_mode(new_ftp_attr,
                    GLOBUS_FTP_CONTROL_MODE_EXTENDED_BLOCK);

                state->source.attr->ftp_attr = new_ftp_attr;
            }

            globus_l_gass_copy_perf_setup_ftp_callback(handle->performance);
        }

        handle->external_third_party = GLOBUS_TRUE;
        result = globus_ftp_client_third_party_transfer(
	    &(handle->ftp_dest_handle),
	    source_url,
	    state->source.attr->ftp_attr,
	    dest_url,
	    state->dest.attr->ftp_attr,
            GLOBUS_NULL,
	    globus_l_gass_copy_ftp_transfer_callback,
	    (void *) handle);

	if (result != GLOBUS_SUCCESS)
	{
	    /* do some error handling */
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	    globus_libc_fprintf(stderr,
              "third_party_transfer() was not GLOBUS_SUCCESS! it returned %d\n",
              result);
#endif
	    goto error_result_exit;
	}
	else
	{
	    handle->status = GLOBUS_GASS_COPY_STATUS_TRANSFER_IN_PROGRESS;
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	    globus_libc_fprintf(stderr,
                "third_party_transfer() returned GLOBUS_SUCCESS\n");
#endif
	}
    }
    else
    {
        /* At least one of the urls is not ftp, (or thirdparty transfers
         * have been turned off) so we have to do the copy ourselves.
         */
	result = globus_l_gass_copy_transfer_start(handle);
	if (result != GLOBUS_SUCCESS)
	{
	    /* free the state */
	    if(handle->state)
	    {
		globus_l_gass_copy_state_free(handle->state);
		handle->state = GLOBUS_NULL;
	    }
	    goto error_result_exit;
	}
    }

    return GLOBUS_SUCCESS;

error_exit:
    if(handle)
	handle->status = GLOBUS_GASS_COPY_STATUS_DONE_FAILURE;
    err = globus_error_construct_string(
	GLOBUS_GASS_COPY_MODULE,
	GLOBUS_NULL,
	"[%s]: BAD_PARAMETER, argument %d cannot be NULL",
	myname,
	bad_param);
    return globus_error_put(err);

error_result_exit:
    handle->status = GLOBUS_GASS_COPY_STATUS_DONE_FAILURE;
    return result;
}/* globus_gass_copy_register_url_to_url() */

/**
 * Transfer data from source URL to an IO handle (non-blocking)
 *
 * This functions initiates a transfer from source URL to an IO handle,
 * then returns immediately.
 *
 * When the transfer is completed or if the
 * transfer is aborted, the callback_func will be invoked with the final
 * status of the transfer.
 *
 * @param handle
 *        The handle to perform the copy operation
 * @param source_url
 *        transfer data from this URL
 * @param source_attr
 *        Attributes describing how the transfer form the source should be done
 * @param dest_handle
 *        transfer data to this IO handle
 * @param callback_func
 *        Callback to be invoked once the transfer is completed.
 * @param callback_arg
 *        Argument to be passed to the callback_func.
 *
 *
 * @return
 *         This function returns GLOBUS_SUCCESS if the transfer was initiated
 *         successfully, or a result pointing to an
 *         object of one of the the following error types:
 * @retval GLOBUS_GASS_COPY_ERROR_TYPE_NULL_PARAMETER
 *         The handle was equal to GLOBUS_NULL, so the transfer could not
 *         processed.
 * @retval GLOBUS_GASS_COPY_ERROR_TYPE_next_error
 *         next error description
 *
 * @see globus_gass_copy_register_url_to_url(),
 *      globus_gass_copy_register_handle_to_url()
 */

globus_result_t
globus_gass_copy_register_url_to_handle(
    globus_gass_copy_handle_t * handle,
    char * source_url,
    globus_gass_copy_attr_t * source_attr,
    globus_io_handle_t * dest_handle,
    globus_gass_copy_callback_t callback_func,
    void * callback_arg)
{
    globus_object_t * err = GLOBUS_ERROR_NO_INFO;
    globus_result_t result;
    globus_gass_copy_state_t * state;
    globus_gass_copy_url_mode_t source_url_mode;
    int bad_param;
    static char * myname="globus_gass_copy_register_url_to_handle";

    /* Check arguments for validity */
    if(handle == GLOBUS_NULL)
    {
	bad_param=1;
	goto error_exit;
    }
    if(source_url == GLOBUS_NULL)
    {
	bad_param=2;
	goto error_exit;
    }
    if(dest_handle == GLOBUS_NULL)
    {
	bad_param=4;
	goto error_exit;
    }

    /* return an error if a transfer is already in progress */
    if(handle->status > GLOBUS_GASS_COPY_STATUS_NONE &&
       handle->status < GLOBUS_GASS_COPY_STATUS_DONE_SUCCESS)
    {
      err = globus_error_construct_string(
	GLOBUS_GASS_COPY_MODULE,
	GLOBUS_NULL,
	"[%s]: There is a transfer already active on this handle",
	myname);
      return globus_error_put(err);
    }

    result = globus_gass_copy_get_url_mode(
	source_url,
	&source_url_mode);

    if(result != GLOBUS_SUCCESS) goto error_result_exit;

    if ( source_url_mode == GLOBUS_GASS_COPY_URL_MODE_UNSUPPORTED)
    {
	handle->status = GLOBUS_GASS_COPY_STATUS_FAILURE;
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: %s,  GLOBUS_GASS_COPY_URL_MODE_UNSUPPORTED",
	    myname,
	    source_url);

	handle->status = GLOBUS_GASS_COPY_STATUS_DONE_FAILURE;

	return globus_error_put(err);
    }

    /* Initialize the state for this transfer */
    result = globus_l_gass_copy_state_new(handle);
    if(result != GLOBUS_SUCCESS) goto error_result_exit;

    state = handle->state;
    state->cancel = GLOBUS_I_GASS_COPY_CANCEL_FALSE;
    /*store the user's callback and argument */
    handle->user_callback = callback_func;
    handle->callback_arg = callback_arg;

    result = globus_l_gass_copy_target_populate(
	handle,
	&(state->source),
	&source_url_mode,
	source_url,
	source_attr);
    if(result != GLOBUS_SUCCESS) goto error_result_exit;
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr, "source target populated\n");
#endif
    result = globus_l_gass_copy_io_target_populate(
	handle,
	&(state->dest),
	dest_handle);
    if(result != GLOBUS_SUCCESS) goto error_result_exit;

#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr, "dest target populated\n");
#endif

    result = globus_l_gass_copy_transfer_start(handle);
    if (result != GLOBUS_SUCCESS)
    {
	goto error_result_exit;
    }

    return GLOBUS_SUCCESS;

error_exit:
    if(handle)
	handle->status = GLOBUS_GASS_COPY_STATUS_DONE_FAILURE;
    err = globus_error_construct_string(
	GLOBUS_GASS_COPY_MODULE,
	GLOBUS_NULL,
	"[%s]: BAD_PARAMETER, argument %d cannot be NULL",
	myname,
	bad_param);
    return globus_error_put(err);

error_result_exit:
    handle->status = GLOBUS_GASS_COPY_STATUS_DONE_FAILURE;
    return result;
} /* globus_gass_copy_register_url_to_handle() */


/**
 * Transfer data from an IO handle to destination URL  (non-blocking)
 *
 * This functions initiates a transfer from an IO handle to destination URL,
 * then returns immediately.
 *
 * When the transfer is completed or if the
 * transfer is aborted, the callback_func will be invoked with the final
 * status of the transfer.
 *
 * @param handle
 *        The handle to perform the copy operation
 * @param source_handle
 *        transfer data from this IO handle
 * @param dest_url
 *        transfer data to this URL
 * @param dest_attr
 *        Attributes describing how the transfer to the destination should be
 *        done
 * @param callback_func
 *        Callback to be invoked once the transfer is completed.
 * @param callback_arg
 *        Argument to be passed to the callback_func.
 *
 *
 * @return
 *         This function returns GLOBUS_SUCCESS if the transfer was initiated
 *         successfully, or a result pointing to an
 *         object of one of the the following error types:
 * @retval GLOBUS_GASS_COPY_ERROR_TYPE_NULL_PARAMETER
 *         The handle was equal to GLOBUS_NULL, so the transfer could not
 *         processed.
 * @retval GLOBUS_GASS_COPY_ERROR_TYPE_next_error
 *         next error description
 *
 * @see globus_gass_copy_register_url_to_url(),
 *      globus_gass_copy_register_url_to_handle()
 */
globus_result_t
globus_gass_copy_register_handle_to_url(
    globus_gass_copy_handle_t * handle,
    globus_io_handle_t * source_handle,
    char * dest_url,
    globus_gass_copy_attr_t * dest_attr,
    globus_gass_copy_callback_t callback_func,
    void * callback_arg)
{
    globus_object_t * err = GLOBUS_ERROR_NO_INFO;
    globus_result_t result;
    globus_gass_copy_state_t * state;
    globus_gass_copy_url_mode_t dest_url_mode;
    int bad_param;
    static char * myname="globus_gass_copy_register_handle_to_url";

    /* Check arguments for validity */
    if(handle == GLOBUS_NULL)
    {
	bad_param=1;
	goto error_exit;
    }
    if(source_handle == GLOBUS_NULL)
    {
	bad_param=2;
	goto error_exit;
    }
    if(dest_url == GLOBUS_NULL)
    {
	bad_param=3;
	goto error_exit;
    }

    /* return an error if a transfer is already in progress */
    if(handle->status > GLOBUS_GASS_COPY_STATUS_NONE &&
       handle->status < GLOBUS_GASS_COPY_STATUS_DONE_SUCCESS)
    {
      err = globus_error_construct_string(
	GLOBUS_GASS_COPY_MODULE,
	GLOBUS_NULL,
	"[%s]: There is a transfer already active on this handle",
	myname);
      return globus_error_put(err);
    }

    result = globus_gass_copy_get_url_mode(
	dest_url,
	&dest_url_mode);
    if(result != GLOBUS_SUCCESS) goto error_result_exit;

    if ( dest_url_mode == GLOBUS_GASS_COPY_URL_MODE_UNSUPPORTED)
    {
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: %s,  GLOBUS_GASS_COPY_URL_MODE_UNSUPPORTED",
	    myname,
	    dest_url);

	handle->status = GLOBUS_GASS_COPY_STATUS_DONE_FAILURE;

	return globus_error_put(err);
    }

    /* Initialize the state for this transfer */
    result = globus_l_gass_copy_state_new(handle);
    if(result != GLOBUS_SUCCESS) goto error_result_exit;

    state = handle->state;
    state->cancel = GLOBUS_I_GASS_COPY_CANCEL_FALSE;
    /*store the user's callback and argument */
    handle->user_callback = callback_func;
    handle->callback_arg = callback_arg;

    result = globus_l_gass_copy_io_target_populate(
	handle,
	&(state->source),
	source_handle);
    if(result != GLOBUS_SUCCESS) goto error_result_exit;
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr, "source target populated\n");
#endif
    result = globus_l_gass_copy_target_populate(
	handle,
	&(state->dest),
	&dest_url_mode,
	dest_url,
	dest_attr);

    if(result != GLOBUS_SUCCESS) goto error_result_exit;
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr, "dest target populated\n");
#endif
    result = globus_l_gass_copy_transfer_start(handle);
    if (result != GLOBUS_SUCCESS)
    {
	goto error_result_exit;
    }

    return GLOBUS_SUCCESS;

error_exit:
    if(handle)
	handle->status = GLOBUS_GASS_COPY_STATUS_DONE_FAILURE;
    err = globus_error_construct_string(
	GLOBUS_GASS_COPY_MODULE,
	GLOBUS_NULL,
	"[%s]: BAD_PARAMETER, argument %d cannot be NULL",
	myname,
	bad_param);
    return globus_error_put(err);
error_result_exit:
    handle->status = GLOBUS_GASS_COPY_STATUS_DONE_FAILURE;
    return result;
} /* globus_gass_copy_register_handle_to_url */

/************************************************************
 * Caching url state
 ************************************************************/

/**
 * Cache connections to an FTP or GSIFTP server.
 *
 * Explicitly cache connections to URL server. When
 * an URL is cached, the connection
 * to the URL server will not be closed after a file transfer completes.
 *
 * @param handle
 *        Handle which will contain a cached connection to the URL server.
 * @param url
 *        The URL of the FTP or GSIFTP server to cache.
 *
 * @return
 *       This function returns GLOBUS_SUCCESS if successful, or a
 *       globus_result_t indicating the error that occurred.
 *
 */
globus_result_t
globus_gass_copy_cache_url_state(
    globus_gass_copy_handle_t * handle,
    char * url)
{
    globus_result_t result;
    globus_url_t url_info;
    globus_object_t * err;
    static char * myname="globus_gass_copy_cache_url_state";

    if(handle != GLOBUS_NULL)
    {
	globus_url_parse(url, &url_info);
	if (   (strcmp(url_info.scheme, "ftp") == 0)
	       || (strcmp(url_info.scheme, "gsiftp") == 0)    )
	{
	    result = globus_ftp_client_handle_cache_url_state(
		&handle->ftp_source_handle,
		url);
            if (result == GLOBUS_SUCCESS)
            {
	        result = globus_ftp_client_handle_cache_url_state(
                           &handle->ftp_dest_handle,
                           url);
	    }
	}
	else
	{
	    err = globus_error_construct_string(
		GLOBUS_GASS_COPY_MODULE,
		GLOBUS_NULL,
              "[%s]: BAD_URL_SCHEME, url: %s, only ftp or gsiftp can be cached",
		myname,
		url);
	    return globus_error_put(err);
	}

        globus_url_destroy(&url_info);

    }
    else
    { /* handle == GLOBUS_NULL */
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: BAD_PARAMETER, handle is NULL",
	    myname);
	return globus_error_put(err);
    }

    return result;

} /* globus_gass_copy_cache_url_state() */

/**
 * Remove a cached connection to an FTP or GSIFTP server.
 *
 * Explicitly remove a cached connection to an FTP or GSIFTP server.
 * If an idle connection to an FTP server exists, it will be closed.
 *
 * @param handle
 *        Handle which contains a cached connection to the URL server.
 * @param url
 *        The URL of the FTP or GSIFTP server to remove.
 *
 * @return
 *       This function returns GLOBUS_SUCCESS if successful, or a
 *       globus_result_t indicating the error that occurred.
 */
globus_result_t
globus_gass_copy_flush_url_state(
    globus_gass_copy_handle_t * handle,
    char * url)
{
    globus_result_t result;
    globus_url_t url_info;
    globus_object_t * err;
    static char * myname="globus_gass_copy_flush_url_state";

    if(handle != GLOBUS_NULL)
    {
	globus_url_parse(url, &url_info);
	if (   (strcmp(url_info.scheme, "ftp") == 0)
	       || (strcmp(url_info.scheme, "gsiftp") == 0)    )
	{
	    result = globus_ftp_client_handle_flush_url_state(
	                &handle->ftp_source_handle,
	                url);
            if (result == GLOBUS_SUCCESS)
            {
                result = globus_ftp_client_handle_flush_url_state(
                               &handle->ftp_dest_handle,
                               url);
            }
	}
	else
	{
	    err = globus_error_construct_string(
	      GLOBUS_GASS_COPY_MODULE,
	      GLOBUS_NULL,
	      "[%s]: BAD_URL_SCHEME, url: %s, only ftp or gsiftp can be cached",
	      myname,
	      url);
	    return globus_error_put(err);
	}

        globus_url_destroy(&url_info);

    }
    else
    { /* handle == GLOBUS_NULL */
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: BAD_PARAMETER, handle is NULL",
	    myname);
	return globus_error_put(err);
    }
    return result;
} /* globus_gass_copy_flush_url_state() */


/************************************************************
 * User pointers on handles
 ************************************************************/

/**
 * Set a pointer in the handle to point at user-allocated memory.
 */
globus_result_t
globus_gass_copy_set_user_pointer(
    globus_gass_copy_handle_t * handle,
    void * user_pointer)
{
    globus_object_t *err;
    static char * myname="globus_gass_copy_set_user_pointer";

    if(handle)
    {
	handle->user_pointer = user_pointer;
	return GLOBUS_SUCCESS;
    }
    else
    {
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: BAD_PARAMETER, handle is NULL",
	    myname);
	return globus_error_put(err);
    }
} /* globus_gass_copy_set_user_pointer() */

/**
 * Get the pointer in the handle that points to user-allocated memory.
 */
globus_result_t
globus_gass_copy_get_user_pointer(
    globus_gass_copy_handle_t * handle,
    void ** user_data)
{
    globus_object_t *err;
    static char * myname="globus_gass_copy_get_user_pointer";

    if (handle)
    {
	*user_data = handle->user_pointer;
	return GLOBUS_SUCCESS;
    }
    else
    {
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: BAD_PARAMETER, handle is NULL",
	    myname);
	return globus_error_put(err);
    }
}

/**
 * Cancel the current transfer associated with this handle,
 */
globus_result_t
globus_gass_copy_cancel(
     globus_gass_copy_handle_t * handle,
     globus_gass_copy_callback_t cancel_callback,
     void * cancel_callback_arg)
{
     globus_i_gass_copy_cancel_t * source_cancel_info = GLOBUS_NULL;
     globus_i_gass_copy_cancel_t * dest_cancel_info = GLOBUS_NULL;
     globus_result_t result;
     globus_result_t source_result = GLOBUS_SUCCESS;
     globus_result_t dest_result = GLOBUS_SUCCESS;
     globus_object_t * err;
     static char * myname="globus_gass_copy_cancel";

#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "starting globus_gass_copy_cancel()\n");
#endif

    if (handle->status == GLOBUS_GASS_COPY_STATUS_NONE)
    {
	err = globus_error_construct_string(
		 GLOBUS_GASS_COPY_MODULE,
		 GLOBUS_NULL,
		 "[%s]: No transfers have been initiated using this handle",
		 myname);

        return globus_error_put(err);
    }

    if (handle->status == GLOBUS_GASS_COPY_STATUS_DONE)
    {
	err = globus_error_construct_string(
		 GLOBUS_GASS_COPY_MODULE,
		 GLOBUS_NULL,
		 "[%s]: The last transfer has already completed.",
		 myname);

        return globus_error_put(err);
    }


    if (handle->state->cancel == GLOBUS_I_GASS_COPY_CANCEL_CALLED)
    {
	if(handle->status == GLOBUS_GASS_COPY_STATUS_CANCEL)
	{
	    err = globus_error_construct_string(
		GLOBUS_GASS_COPY_MODULE,
		GLOBUS_NULL,
		"[%s]: The last transfer has already been canceled.",
		myname);

	    return globus_error_put(err);
	}

	if(handle->status == GLOBUS_GASS_COPY_STATUS_FAILURE)
	{
	    err = globus_error_construct_string(
		GLOBUS_GASS_COPY_MODULE,
		GLOBUS_NULL,
		"[%s]: A failure has already been detected in the last transfer.",
		myname);

	    return globus_error_put(err);
	}
    }

    handle->state->cancel = GLOBUS_I_GASS_COPY_CANCEL_CALLED;
    if (handle->status != GLOBUS_GASS_COPY_STATUS_FAILURE)
    {
        handle->status = GLOBUS_GASS_COPY_STATUS_CANCEL;
    }

    /**
     * store the cancel_callback and cancel_callback_arg in the handle.
     * Needed because the ftp callback will be the one given from the
     * original globus_ftp_client_third_party_transfer() call.
     */
    handle->user_cancel_callback = cancel_callback;
    handle->cancel_callback_arg  = cancel_callback_arg;

    if (handle->external_third_party)
    {
        result = globus_ftp_client_abort(&handle->ftp_dest_handle);
    }
    else
    {
	source_cancel_info = (globus_i_gass_copy_cancel_t *)
	    globus_libc_malloc(sizeof(globus_i_gass_copy_cancel_t));
	source_cancel_info->handle = handle;
	source_cancel_info->canceling_source = GLOBUS_TRUE;

	dest_cancel_info = (globus_i_gass_copy_cancel_t *)
	    globus_libc_malloc(sizeof(globus_i_gass_copy_cancel_t));
	dest_cancel_info->handle = handle;
	dest_cancel_info->canceling_source = GLOBUS_FALSE;

	if(handle->state->source.status != GLOBUS_I_GASS_COPY_TARGET_DONE &&
	   handle->state->source.status != GLOBUS_I_GASS_COPY_TARGET_INITIAL)
	    source_result = globus_l_gass_copy_target_cancel(source_cancel_info);

        if (handle->state != GLOBUS_NULL)
        {
            if(handle->state->dest.status != GLOBUS_I_GASS_COPY_TARGET_DONE &&
               handle->state->dest.status != GLOBUS_I_GASS_COPY_TARGET_INITIAL)
            {
	        dest_result = globus_l_gass_copy_target_cancel(dest_cancel_info);
            }
        }

	if(source_result != GLOBUS_SUCCESS)
	{
	    result = source_result;
	}
	else if(dest_result != GLOBUS_SUCCESS)
	{
	    result = dest_result;
	}
	else
	{
	    result = GLOBUS_SUCCESS;
	}
/*
        globus_libc_free(dest_cancel_info);
        globus_libc_free(source_cancel_info);
*/
    }

#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "leaving globus_gass_copy_cancel()\n");
#endif

    return result;

}

/**
 * Cancel the source or destination transfer in progress.
 */
globus_result_t
globus_l_gass_copy_target_cancel(
    globus_i_gass_copy_cancel_t * cancel_info)
{
    globus_result_t result = GLOBUS_SUCCESS;
    globus_i_gass_copy_target_t * target;
    globus_object_t * err;
    static char * myname="globus_l_gass_copy_target_cancel";
    int rc = 0;
    int req_status;

/* should check for these errors
    if (cancel_info == GLOBUS_NULL)
    {
       set error
    }
    if (cancel_info->handle == GLOBUS_NULL ||
        cancel_info->handle.state == GLOBUS_NULL)
    {
       set error
    }
*/

    if (cancel_info->canceling_source)
    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "_target_cancel(): cancelling source\n");
#endif
       target = &(cancel_info->handle->state->source);
    }
    else
    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "_target_cancel(): cancelling dest\n");
#endif
	target = &(cancel_info->handle->state->dest);
    }

    switch (target->mode)
    {
        case GLOBUS_GASS_COPY_URL_MODE_FTP:
             result = globus_ftp_client_abort(target->data.ftp.handle);
             if(cancel_info->handle->performance &&
                !cancel_info->canceling_source)
             {
                globus_l_gass_copy_perf_cancel_ftp_callback(
                    cancel_info->handle->performance);
             }

             if (result != GLOBUS_SUCCESS)
             {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
                globus_libc_fprintf(stderr,
                   "target_cancel(): _ftp_client_abort()  returned an error\n");
		globus_libc_fprintf(stderr, "target_cancel(): error = %s\n",
		   globus_object_printable_to_string(globus_error_get(result)));
                globus_libc_fprintf(stderr, "    resetting to SUCCESS\n");
#endif
                 result = GLOBUS_SUCCESS;
             }

             break;
        case GLOBUS_GASS_COPY_URL_MODE_GASS:

	    req_status =
		globus_gass_transfer_request_get_status(target->data.gass.request);
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	    globus_libc_fprintf(stderr,
                   "target_cancel: gass_request_status = %d\n", req_status);
#endif
	    if(req_status != GLOBUS_GASS_TRANSFER_REQUEST_FAILED)
            {
		rc = globus_gass_transfer_fail(
                      target->data.gass.request,
                      globus_l_gass_copy_gass_transfer_cancel_callback,
                      cancel_info);
		if (rc != GLOBUS_SUCCESS)
		{
		     err = globus_error_construct_string(
		     GLOBUS_GASS_COPY_MODULE,
		     GLOBUS_NULL,
		     "[%s]: %s globus_gass_transfer_request_fail returned an error code of: %d",
		     myname,
		     target->url,
		     rc);
		     globus_i_gass_copy_set_error(cancel_info->handle, err);

		     result = globus_error_put(err);
		}
		else
		{
		    globus_l_gass_copy_generic_cancel(cancel_info);
		}
	    }
	    else
	    {
		globus_gass_transfer_request_destroy(target->data.gass.request);
		globus_l_gass_copy_generic_cancel(cancel_info);
	    }

             break;
        case GLOBUS_GASS_COPY_URL_MODE_IO:
             result =  globus_io_register_cancel(
                              target->data.io.handle,
                              GLOBUS_FALSE,
                              globus_l_gass_copy_io_cancel_callback,
                              cancel_info);
             break;
    }
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    if(result != GLOBUS_SUCCESS)
	globus_libc_fprintf(stderr,
            "[%s]: error trying to cancel one of the targets\n",
            myname);
#endif
    return result;
}

void
globus_l_gass_copy_gass_transfer_cancel_callback(
    void * callback_arg,
    globus_gass_transfer_request_t request)
{
    globus_gass_transfer_request_status_t status;
    globus_object_t *                     err;
    globus_i_gass_copy_cancel_t *         cancel_info
	= (globus_i_gass_copy_cancel_t *) callback_arg;

#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr,
            "starting _gass_transfer_cancel_callback()\n");
#endif

    status = globus_gass_transfer_request_get_status(request);

    if (status != GLOBUS_SUCCESS)
    {
        err = globus_error_construct_string(
              GLOBUS_GASS_COPY_MODULE,
              GLOBUS_NULL,
              "[%s]: gass_transfer_request_status: %d",
                     "globus_gass_transfer_fail",
                     status);
        globus_i_gass_copy_set_error(cancel_info->handle, err);
    }

    globus_l_gass_copy_generic_cancel(cancel_info);
}

void
globus_l_gass_copy_io_cancel_callback(
    void * callback_arg,
    globus_io_handle_t * handle,
    globus_result_t result)
{
    globus_i_gass_copy_cancel_t * cancel_info
        = (globus_i_gass_copy_cancel_t *) callback_arg;

#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "starting _io_cancel_callback()\n");
#endif

/*  what to do if we get an error ??
 *  if (result != GLOBUS_SUCCESS)
 */

    globus_l_gass_copy_generic_cancel(cancel_info);
}

void
globus_l_gass_copy_generic_cancel(
    globus_i_gass_copy_cancel_t * cancel_info)
{
    globus_gass_copy_handle_t * handle = cancel_info->handle;
    globus_bool_t  all_done = GLOBUS_FALSE;
    globus_object_t * err = GLOBUS_NULL;

#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "starting _gass_copy_generic_cancel()\n");
#endif
    globus_mutex_lock(&(handle->state->mutex));

    if (cancel_info->canceling_source)
    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "_generic_cancel() source\n");
#endif
	handle->state->source.status = GLOBUS_I_GASS_COPY_TARGET_DONE;
        if (handle->state->dest.status == GLOBUS_I_GASS_COPY_TARGET_DONE ||
	    handle->state->dest.status == GLOBUS_I_GASS_COPY_TARGET_INITIAL)
        {
           all_done = GLOBUS_TRUE;
        }
    }
    else
    {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "_generic_cancel() dest\n");
#endif
	handle->state->dest.status = GLOBUS_I_GASS_COPY_TARGET_DONE;
        if (handle->state->source.status == GLOBUS_I_GASS_COPY_TARGET_DONE ||
	    handle->state->source.status == GLOBUS_I_GASS_COPY_TARGET_INITIAL)
        {
           all_done = GLOBUS_TRUE;
        }

        if(handle->performance)
        {
            globus_l_gass_copy_perf_cancel_local_callback(handle->performance);
        }
    }

    globus_mutex_unlock(&(handle->state->mutex));
#ifdef GLOBUS_I_GASS_COPY_DEBUG
    globus_libc_fprintf(stderr, "_generic_cancel() before all done\n");
#endif

    if (all_done)
    {
	globus_l_gass_copy_wait_for_ftp_callbacks(handle);

	globus_l_gass_copy_state_free(handle->state);
	handle->state = GLOBUS_NULL;

#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr, "globus_l_gass_copy_generic_cancel():\n");
	globus_libc_fprintf(stderr,
            "     ...check to call user/cancel callbacks.\n");
#endif

	/* set the final status of the transfer */
	switch(handle->status)
	{
	case GLOBUS_GASS_COPY_STATUS_FAILURE:
	  handle->status = GLOBUS_GASS_COPY_STATUS_DONE_FAILURE;
	  break;
	case GLOBUS_GASS_COPY_STATUS_CANCEL:
	  handle->status = GLOBUS_GASS_COPY_STATUS_DONE_CANCELLED;
	  break;
	}

        err = handle->err;
	handle->err = GLOBUS_NULL;
	
	if(handle->user_cancel_callback != GLOBUS_NULL)
        {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	globus_libc_fprintf(stderr,
            "        ...calling user cancel callback.\n");
#endif
	    handle->user_cancel_callback(
		handle->cancel_callback_arg,
		handle,
		err);
        }

	if(handle->user_callback != GLOBUS_NULL)
        {
#ifdef GLOBUS_I_GASS_COPY_DEBUG
	    globus_libc_fprintf(stderr, "        ...calling user callback.\n");
#endif
	    handle->user_callback(
		handle->callback_arg,
		handle,
		err);
        }
	/* if an error object was created, free it */

        if (err)
        {
	    globus_object_free(err);
        }

    } /* if (all_done) */

    return;
}


/************************************************************
 * Attributes
 ************************************************************/
#ifdef USE_FTP_ATTRS
/**
 * Set TCP buffer/window size
 */
globus_result_t
globus_gass_copy_attr_set_tcpbuffer(
    globus_gass_copy_attr_t * attr,
    globus_ftp_control_tcpbuffer_t * tcpbuffer_info)
{

/* how should we set errors */

    if (attr == GLOBUS_NULL)
        return GLOBUS_GASS_COPY_ERROR_NULL_ATTR;

    if (attr == GLOBUS_NULL)
        return GLOBUS_GASS_COPY_ERROR_NULL_TCPBUFFER;

/* or */

    if (attr == GLOBUS_NULL)
    {
        return globus_error_put(
           globus_gass_copy_error_construct_null_parameter(
              GLOBUS_GASS_COPY_MODULE,
              GLOBUS_NULL,
              "attr",
              1,
              "globus_gass_copy_attr_set_tcpbuffer");
    }

    if (tcpbuffer_info == GLOBUS_NULL)
    {
        return globus_error_put(
           globus_gass_copy_error_construct_null_parameter(
              GLOBUS_GASS_COPY_MODULE,
              GLOBUS_NULL,
              "tcpbuffer_info",
              2,
              "globus_gass_copy_attr_set_tcpbuffer");
    }

    attr->tcpbuffer_info = *tcpbuffer_info;
}

/**
 * Set parallelism info
 */
globus_result_t
globus_gass_copy_attr_set_parallelism(
    globus_gass_copy_attr_t * attr,
    globus_ftp_control_parallelism_t * parallelism_info)
{
    attr->parallel_info = *parallel_info;
}

/**
 * Set striping info
 */
globus_result_t
globus_gass_copy_attr_set_striping(
    globus_gass_copy_attr_t * attr,
    globus_ftp_control_striping_t * striping_info)
{
    attr->striping_info = *striping_info;
}

/**
 * Set authorization info
 */
globus_result_t
globus_gass_copy_attr_set_authorization(
    globus_gass_copy_attr_t * attr,
    globus_io_authorization_t * authorization_info)
{
    attr->authorization_info = *authorization_info;
}

/**
 * Set secure channel info
 */
globus_result_t
globus_gass_copy_attr_set_secure_channel(
    globus_gass_copy_attr_t * attr,
    globus_io_secure_channel_t * secure_channel_info)
{
    attr->secure_channel_info = *secure_channel_info;
}

#endif

#ifndef GLOBUS_DONT_DOCUMENT_INTERNAL
/**
 * Duplicate the passed in attribute structure.
 */
globus_result_t
globus_i_gass_copy_attr_duplicate(globus_gass_copy_attr_t ** attr)
{
    globus_gass_copy_attr_t * new_attr;
    globus_ftp_client_operationattr_t * new_ftp_attr;
    globus_object_t * err;
    static char * myname="globus_i_gass_copy_attr_duplicate";
#if 0
    if ( (attr == GLOBUS_NULL) || (*attr == GLOBUS_NULL) )
    {
	err = globus_error_construct_string(
	    GLOBUS_GASS_COPY_MODULE,
	    GLOBUS_NULL,
	    "[%s]: BAD_PARAMETER, attr==GLOBUS_NULL, or *attr==GLOBUS_NULL",
	    myname);

	return globus_error_put(err);
    }

    new_attr = (globus_gass_copy_attr_t *)
	globus_libc_malloc(sizeof(globus_gass_copy_attr_t));
    globus_gass_copy_attr_init(new_attr);

    new_ftp_attr = (globus_ftp_client_operationattr_t *)
	globus_libc_malloc(sizeof(globus_ftp_client_operationattr_t));

    globus_ftp_cient_attr_copy(new_ftp_attr, *(attr)->ftp_attr);
    /* new_attr = *attr; */
    *attr = new_attr;
#endif
    return GLOBUS_SUCCESS;
} /* globus_i_gass_copy_attr_duplicate */

#endif /* GLOBUS_DONT_DOCUMENT_INTERNAL */

/************************************************************
 * Example
 ************************************************************

globus_gass_copy_t handle;
globus_gass_copy_init(&handle);
globus_gass_copy_cache_url_state(
    &handle,
    "gsiftp://pitcairn.mcs.anl.gov/");
globus_gass_copy_url_to_url(
    &handle,
    "gsiftp://pitcairn.mcs.anl.gov/tmp/foo",
    "gsiftp://lemon.mcs.anl.gov/tmp/foo");
globus_gass_copy_url_to_url(
    &handle,
    "gsiftp://pitcairn.mcs.anl.gov/tmp/foo",
    "gsiftp://tuva.mcs.anl.gov/tmp/foo");
globus_gass_copy_url_to_url(
    &handle,
    "gsiftp://pitcairn.mcs.anl.gov/tmp/foo",
    "http://tuva.mcs.anl.gov/tmp/foo");
globus_gass_copy_url_to_iohandle(
    &handle,
    "gsiftp://pitcairn.mcs.anl.gov/tmp/foo",
    io_handle);
globus_gass_copy_destroy(&handle);

*/

/************************************************************
 * Example Attributes
 ************************************************************

globus_io_authorization_t a;
globus_io_authorization_t b;

a.mode = GLOBUS_IO_AUTHORIZATION_MODE_IDENTITY;
strcpy(a.data.identity.subject, "foo");

globus_gass_copy_attr_set_authorization(attr, &a);
globus_gass_copy_attr_get_authorization(attr, &b);

b.mode = ...

globus_gass_copy_attr_set_authorization(attr2, &b);

typedef struct globus_gass_copy_attr_s
{
    globus_io_authorization_t a;
    ...
} globus_gass_copy_attr_t;

*/

/*
globus_gass_copy_t handle;
globus_gass_copy_init(&handle);

globus_gass_copy_attribute_setup_ftp(handle, ftp_attr);
globus_gass_copy_attribute_setup_io(handle, io_attr);
globus_gass_copy_attribute_setup_gass(handle, io_attr);

globus_gass_copy_cache_url_state(
    &handle,
    "gsiftp://pitcairn.mcs.anl.gov/");
globus_gass_copy_url_to_url(
    &handle,
    "gsiftp://pitcairn.mcs.anl.gov/tmp/foo",
    "gsiftp://lemon.mcs.anl.gov/tmp/foo");

**/
