//
// Created by 熊嘉晟 on 2024/6/23.
//

#include "rdma_write_respond_dep.h"

DOCA_LOG_REGISTER(RDMA_WRITE_RESPONDER::SAMPLE);

doca_error_t write_read_connection(struct rdma_config *cfg, struct rdma_resources *resources)
{
    int enter = 0;
    doca_error_t result = DOCA_SUCCESS;

    /* Write the RDMA connection details */
    result = write_file(cfg->local_connection_desc_path,
                        (char *)resources->rdma_conn_descriptor,
                        resources->rdma_conn_descriptor_size);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to write the RDMA connection details: %s", doca_error_get_descr(result));
        return result;
    }

    /* Write the mmap connection details */
    result = write_file(cfg->remote_resource_desc_path,
                        (char *)resources->mmap_descriptor,
                        resources->mmap_descriptor_size);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to write the RDMA mmap details: %s", doca_error_get_descr(result));
        return result;
    }

    DOCA_LOG_INFO("You can now copy %s and %s to the requester",
                  cfg->local_connection_desc_path,
                  cfg->remote_resource_desc_path);
    DOCA_LOG_INFO("Please copy %s from the requester and then press enter", cfg->remote_connection_desc_path);

    /* Wait for enter */
    while (enter != '\r' && enter != '\n')
        enter = getchar();

    /* Read the remote RDMA connection details */
    result = read_file(cfg->remote_connection_desc_path,
                       (char **)&resources->remote_rdma_conn_descriptor,
                       &resources->remote_rdma_conn_descriptor_size);
    if (result != DOCA_SUCCESS)
        DOCA_LOG_ERR("Failed to read the remote RDMA connection details: %s", doca_error_get_descr(result));

    return result;
}

doca_error_t rdma_write_responder_export_and_connect(struct rdma_resources *resources)
{
    doca_error_t result;

    /* Export RDMA connection details */
    result = doca_rdma_export(resources->rdma,
                              &(resources->rdma_conn_descriptor),
                              &(resources->rdma_conn_descriptor_size));
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to export RDMA: %s", doca_error_get_descr(result));
        return result;
    }

    /* Export RDMA mmap */
    result = doca_mmap_export_rdma(resources->mmap,
                                   resources->doca_device,
                                   (const void **)&(resources->mmap_descriptor),
                                   &(resources->mmap_descriptor_size));
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to export DOCA mmap for RDMA: %s", doca_error_get_descr(result));
        return result;
    }

    /* write and read connection details from the requester */
    result = write_read_connection(resources->cfg, resources);
    if (result != DOCA_SUCCESS)
        DOCA_LOG_ERR("Failed to write and read connection details from the requester: %s",
                     doca_error_get_descr(result));

    /* Connect RDMA */
    result = doca_rdma_connect(resources->rdma,
                               resources->remote_rdma_conn_descriptor,
                               resources->remote_rdma_conn_descriptor_size);
    if (result != DOCA_SUCCESS)
        DOCA_LOG_ERR("Failed to connect the responder's RDMA to the requester's RDMA: %s",
                     doca_error_get_descr(result));

    return result;
}

void rdma_write_responder_state_change_callback(const union doca_data user_data,
                                                       struct doca_ctx *ctx,
                                                       enum doca_ctx_states prev_state,
                                                       enum doca_ctx_states next_state)
{
    struct rdma_resources *resources = (struct rdma_resources *)user_data.ptr;
    int enter = 0;
    char buffer[MAX_BUFF_SIZE];
    doca_error_t result = DOCA_SUCCESS;

    (void)prev_state;
    (void)ctx;

    switch (next_state) {
        case DOCA_CTX_STATE_STARTING:
            DOCA_LOG_INFO("RDMA context entered starting state");

            result = rdma_write_responder_export_and_connect(resources);
            if (result != DOCA_SUCCESS)
                DOCA_LOG_ERR("rdma_write_responder_export_and_connect() failed: %s",
                             doca_error_get_descr(result));
            else
                DOCA_LOG_INFO("RDMA context finished initialization");
            break;
        case DOCA_CTX_STATE_RUNNING:
            DOCA_LOG_INFO("RDMA context is running");

            /* Wait for enter which means that the requester has finished writing */
            DOCA_LOG_INFO("Wait till the requester has finished writing and press enter");
            while (enter != '\r' && enter != '\n')
                enter = getchar();

            /* Initialize buffer to zeros */
            memset(buffer, 0, MAX_BUFF_SIZE);

            /* Read the data that was written on the mmap */
            strncpy(buffer, resources->mmap_memrange, MAX_BUFF_SIZE - 1);

            /* Check if the buffer is null terminated and of legal size */
            if (strnlen(buffer, MAX_BUFF_SIZE) == MAX_BUFF_SIZE) {
                DOCA_LOG_ERR("The message that was written by the requester exceeds buffer size %d",
                             MAX_BUFF_SIZE);
                result = DOCA_ERROR_INVALID_VALUE;
                break;
            }

            DOCA_LOG_INFO("Requester has written: \"%s\"", buffer);

            /* Stop context */
            (void)doca_ctx_stop(resources->rdma_ctx);
            break;
        case DOCA_CTX_STATE_STOPPING:
            /**
             * doca_ctx_stop() has been called.
             * In this sample, this happens either due to a failure encountered, in which case doca_pe_progress()
             * will cause any inflight task to be flushed, or due to the successful compilation of the sample flow.
             * In both cases, in this sample, doca_pe_progress() will eventually transition the context to idle
             * state.
             */
            DOCA_LOG_INFO("RDMA context entered into stopping state. Any inflight tasks will be flushed");
            break;
        case DOCA_CTX_STATE_IDLE:
            DOCA_LOG_INFO("RDMA context has been stopped");

            /* We can stop progressing the PE */
            resources->run_pe_progress = false;
            break;
        default:
            break;
    }

    /* If something failed - update that an error was encountered and stop the ctx */
    if (result != DOCA_SUCCESS) {
        DOCA_ERROR_PROPAGATE(resources->first_encountered_error, result);
        (void)doca_ctx_stop(ctx);
    }
}

doca_error_t rdma_write_responder(struct rdma_config *cfg)
{
    struct rdma_resources resources = {0};
    union doca_data ctx_user_data = {0};
    const uint32_t mmap_permissions = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE;
    const uint32_t rdma_permissions = DOCA_ACCESS_FLAG_RDMA_WRITE;
    doca_error_t result, tmp_result;
    struct timespec ts = {
            .tv_sec = 0,
            .tv_nsec = SLEEP_IN_NANOS,
    };

    /* Allocating resources */
    result = allocate_rdma_resources(cfg, mmap_permissions, rdma_permissions, NULL, &resources);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to allocate RDMA Resources: %s", doca_error_get_descr(result));
        return result;
    }

    result = doca_ctx_set_state_changed_cb(resources.rdma_ctx, rdma_write_responder_state_change_callback);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Unable to set state change callback for RDMA context: %s", doca_error_get_descr(result));
        goto destroy_resources;
    }

    /* Include the program's resources in user data of context to be used in callbacks */
    ctx_user_data.ptr = &(resources);
    result = doca_ctx_set_user_data(resources.rdma_ctx, ctx_user_data);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set context user data: %s", doca_error_get_descr(result));
        goto destroy_resources;
    }

    /* Start RDMA context */
    result = doca_ctx_start(resources.rdma_ctx);
    /* DOCA_ERROR_IN_PROGRESS is expected and handled by the state change callback function */
    if (result != DOCA_ERROR_IN_PROGRESS) {
        DOCA_LOG_ERR("Failed to start RDMA context: %s", doca_error_get_descr(result));
        goto destroy_resources;
    }

    /*
     * Run the progress engine which will run the state machine defined in
     * rdma_write_responder_state_change_callback() When the requester finishes writing, the user will signal to
     * stop running the progress engine.
     */
    while (resources.run_pe_progress) {
        if (doca_pe_progress(resources.pe) == 0)
            nanosleep(&ts, &ts);
    }

    /* Assign the result we update in the callbacks */
    result = resources.first_encountered_error;

    destroy_resources:
    tmp_result = destroy_rdma_resources(&resources, cfg);
    if (tmp_result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to destroy DOCA RDMA resources: %s", doca_error_get_descr(tmp_result));
        DOCA_ERROR_PROPAGATE(result, tmp_result);
    }
    return result;
}