#include <CL/cl.h>
#include <glib.h>
#include <dlfcn.h>
#include <signal.h>

#ifdef HAVE_LIBUNWIND
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#endif

typedef struct {
    guint refs;
    gpointer obj;
    gsize size;
} StatItem;

typedef struct {
    GHashTable *contexts;
    GHashTable *queues;
    GHashTable *buffers;
    GHashTable *samplers;
    GHashTable *kernels;
} StatData;

static volatile StatData stat_data_state = { NULL, };

G_LOCK_DEFINE_STATIC (stat_data);

static GList *
get_alive_items (GList *items)
{
    GList *alive = NULL;

    for (GList *it = items; it != NULL; it = g_list_next (it)) {
        StatItem *item = (StatItem *) it->data;

        if (item->refs > 0)
            alive = g_list_append (alive, item);
    }

    return alive;
}

static gsize
total_buffer_size (GList *items)
{
    gsize total_size = 0;

    for (GList *it = items; it != NULL; it = g_list_next (it))
        total_size += ((StatItem *) it->data)->size;

    return total_size;
}

static void
dump_item (GHashTable *table, const gchar *name)
{
    GList *items;
    GList *items_alive;
    gchar *s;

    items = g_hash_table_get_values (table);
    items_alive = get_alive_items (items);

    s = g_strdup_printf ("%-16s%i/%i\n", name, g_list_length (items_alive), g_list_length (items));
    g_print (" %s", s);
    g_free (s);

    g_list_free (items);
    g_list_free (items_alive);
}

static void
dump_info (void)
{
    GList *items;
    GList *items_alive;

    g_print ("\nOpenCL objects alive\n"
             "====================\n");

    G_LOCK (stat_data);

    dump_item (stat_data_state.contexts, "Contexts");
    dump_item (stat_data_state.queues, "Command queues");
    dump_item (stat_data_state.buffers, "Buffers");
    dump_item (stat_data_state.samplers, "Samplers");
    dump_item (stat_data_state.kernels, "Kernels");

    g_print ("\nMemory leaks\n"
             "============\n");

    items = g_hash_table_get_values (stat_data_state.buffers);
    items_alive = get_alive_items (items);

    g_print (" %-16s%zu B\n", "Leaking", total_buffer_size (items_alive));

    g_list_free (items);
    g_list_free (items_alive);

    G_UNLOCK (stat_data);
}

static void
dump_trace (void)
{
#ifdef HAVE_LIBUNWIND
    unw_context_t context;
    unw_cursor_t cursor;

    unw_getcontext (&context);
    unw_init_local (&cursor, &context);

    while (unw_step (&cursor) > 0) {
        gchar name[129];
        unw_word_t off;
        int result;

        result = unw_get_proc_name (&cursor, name, sizeof (name), &off);

        if (result < 0)
            break;

        g_print ("  %s + [0x%08x]\n", name, (unsigned int) off);
    }
#endif
}

static void
_exit_handler (void)
{
    dump_info ();
}

static void
_sig_usr1_handler (int signal)
{
    dump_info ();
}

static StatItem *
stat_item_new (gpointer obj)
{
    StatItem *item;
    item = g_new0 (StatItem, 1);
    item->refs = 1;
    item->obj = obj;
    return item;
}

static void *
get_func (const char *name)
{
    static void *handle = NULL;
    void *func;
    char *error;

    G_LOCK (stat_data);

    if (g_once_init_enter (&handle)) {
        void *_handle;

        _handle = dlopen ("libOpenCL.so.1", RTLD_LAZY);

        if (_handle == NULL)
            g_error ("Failed to open libOpenCL.so.1: %s", dlerror ());

        stat_data_state.contexts = g_hash_table_new (NULL, NULL);
        stat_data_state.queues = g_hash_table_new (NULL, NULL);
        stat_data_state.buffers = g_hash_table_new (NULL, NULL);
        stat_data_state.samplers = g_hash_table_new (NULL, NULL);
        stat_data_state.kernels = g_hash_table_new (NULL, NULL);

        signal (SIGUSR1, _sig_usr1_handler);
        signal (SIGINT, _sig_usr1_handler);
        signal (SIGTERM, _sig_usr1_handler);
        signal (SIGABRT, _sig_usr1_handler);
        signal (SIGSEGV, _sig_usr1_handler);

        atexit (_exit_handler);

        g_once_init_leave (&handle, _handle);
    }

    func = dlsym (handle, name);
    error = dlerror ();

    if (error != NULL)
        g_error ("Failed to find symbol: %s", error);

    G_UNLOCK (stat_data);

    return func;
}

cl_context
clCreateContext (const cl_context_properties *properties,
                 cl_uint num_devices,
                 const cl_device_id *devices,
                 void (CL_CALLBACK *pfn_notify) (const char*, const void *, size_t, void*),
                 void *user_data,
                 cl_int *errcode_ret)
{
    cl_context context;
    StatItem *item;
    cl_context (*clCreateContextReal) (const cl_context_properties *, cl_uint, const cl_device_id *, void (CL_CALLBACK *) (const char *, const void *, size_t, void *), void *, cl_int *);

    clCreateContextReal = get_func ("clCreateContext");
    context = clCreateContextReal (properties, num_devices, devices, pfn_notify, user_data, errcode_ret);
    item = stat_item_new (context);

    G_LOCK (stat_data);
    g_hash_table_insert (stat_data_state.contexts, context, item);
    G_UNLOCK (stat_data);

    return context;
}

cl_int
clRetainContext (cl_context context)
{
    cl_int ret;
    cl_int (* clRetainContextReal) (cl_context);
    StatItem *item;

    clRetainContextReal = get_func ("clRetainContext");
    ret = clRetainContextReal (context);

    G_LOCK (stat_data);

    item = g_hash_table_lookup (stat_data_state.contexts, context);

    if (item != NULL)
        item->refs++;
    else {
        g_print ("clRetainContext: unknown context\n");
        dump_trace ();
    }

    G_UNLOCK (stat_data);

    return ret;
}

cl_int
clReleaseContext (cl_context context)
{
    cl_int ret;
    cl_int (* clReleaseContextReal) (cl_context);
    StatItem *item;

    clReleaseContextReal = get_func ("clReleaseContext");
    ret = clReleaseContextReal (context);

    G_LOCK (stat_data);

    item = g_hash_table_lookup (stat_data_state.contexts, context);

    if (item != NULL) {
        item->refs--;
    }
    else {
        g_print ("clReleaseContext: unknown context\n");
        dump_trace ();
    }

    G_UNLOCK (stat_data);

    return ret;
}

cl_command_queue
clCreateCommandQueue (cl_context context,
                      cl_device_id device,
                      cl_command_queue_properties properties,
                      cl_int *errcode_ret)
{
    cl_command_queue queue;
    StatItem *item;
    cl_command_queue (* clCreateCommandQueueReal) (cl_context, cl_device_id, cl_command_queue_properties, cl_int *);

    clCreateCommandQueueReal = get_func ("clCreateCommandQueue");
    queue = clCreateCommandQueueReal (context, device, properties, errcode_ret);
    item = stat_item_new (queue);

    G_LOCK (stat_data);
    g_hash_table_insert (stat_data_state.queues, queue, item);
    G_UNLOCK (stat_data);

    return queue;
}

cl_int
clRetainCommandQueue (cl_command_queue command_queue)
{
    cl_int ret;
    cl_int (* clRetainCommandQueueReal) (cl_command_queue);
    StatItem *item;

    clRetainCommandQueueReal = get_func ("clRetainCommandQueue");
    ret = clRetainCommandQueueReal (command_queue);

    G_LOCK (stat_data);

    item = g_hash_table_lookup (stat_data_state.queues, command_queue);

    if (item != NULL) {
        item->refs++;
    }
    else {
        g_print ("clRetainCommandQueue: unknown command queue\n");
        dump_trace ();
    }

    G_UNLOCK (stat_data);

    return ret;
}

cl_int
clReleaseCommandQueue (cl_command_queue command_queue)
{
    cl_int ret;
    cl_int (* clReleaseCommandQueueReal) (cl_command_queue);
    StatItem *item;

    clReleaseCommandQueueReal = get_func ("clReleaseCommandQueue");
    ret = clReleaseCommandQueueReal (command_queue);

    G_LOCK (stat_data);

    item = g_hash_table_lookup (stat_data_state.queues, command_queue);

    if (item != NULL) {
        item->refs--;
    }
    else {
        g_print ("clReleaseCommandQueue: unknown command queue %p\n", command_queue);
        dump_trace ();
    }

    G_UNLOCK (stat_data);

    return ret;
}

cl_mem
clCreateBuffer (cl_context context,
                cl_mem_flags flags,
                size_t size,
                void *host_ptr,
                cl_int *errcode_ret)
{
    cl_mem buffer;
    StatItem *item;
    cl_mem (* clCreateBufferReal) (cl_context, cl_mem_flags, size_t, void *, cl_int *);

    clCreateBufferReal = get_func ("clCreateBuffer");
    buffer = clCreateBufferReal (context, flags, size, host_ptr, errcode_ret);
    item = stat_item_new (buffer);
    item->size = size;

    G_LOCK (stat_data);
    g_hash_table_insert (stat_data_state.buffers, buffer, item);
    G_UNLOCK (stat_data);

    return buffer;
}

cl_mem
clCreateImage (cl_context context,
               cl_mem_flags flags,
               const cl_image_format *image_format,
               const cl_image_desc *image_desc,
               void *host_ptr,
               cl_int *errcode_ret)
{
    cl_mem buffer;
    StatItem *item;
    cl_mem (* clCreateImageReal) (cl_context, cl_mem_flags, const cl_image_format *, const cl_image_desc *, void *, cl_int *);

    clCreateImageReal = get_func ("clCreateImage");
    buffer = clCreateImageReal (context, flags, image_format, image_desc, host_ptr, errcode_ret);
    item = stat_item_new (buffer);
    item->size = 1; /* FIXME: compute correct size */

    G_LOCK (stat_data);
    g_hash_table_insert (stat_data_state.buffers, buffer, item);
    G_UNLOCK (stat_data);

    return buffer;
}

cl_mem
clCreateImage2D (cl_context context,
                 cl_mem_flags flags,
                 const cl_image_format *image_format,
                 size_t image_width,
                 size_t image_height,
                 size_t image_row_pitch,
                 void *host_ptr,
                 cl_int *errcode_ret)
{
    cl_mem buffer;
    StatItem *item;
    cl_mem (* clCreateImage2DReal) (cl_context, cl_mem_flags, const cl_image_format *, size_t, size_t, size_t, void *, cl_int *);

    clCreateImage2DReal = get_func ("clCreateImage2D");
    buffer = clCreateImage2DReal (context, flags, image_format, image_width, image_height, image_row_pitch, host_ptr, errcode_ret);
    item = stat_item_new (buffer);
    item->size = image_width * image_height; /* FIXME: compute correct size */

    G_LOCK (stat_data);
    g_hash_table_insert (stat_data_state.buffers, buffer, item);
    G_UNLOCK (stat_data);

    return buffer;
}

cl_mem
clCreateImage3D (cl_context context,
                 cl_mem_flags flags,
                 const cl_image_format *image_format,
                 size_t image_width,
                 size_t image_height,
                 size_t image_depth,
                 size_t image_row_pitch,
                 size_t image_slice_pitch,
                 void *host_ptr,
                 cl_int *errcode_ret)
{
    cl_mem buffer;
    StatItem *item;
    cl_mem (* clCreateImage2DReal) (cl_context, cl_mem_flags, const cl_image_format *, size_t, size_t, size_t, size_t, size_t, void *, cl_int *);

    clCreateImage2DReal = get_func ("clCreateImage2D");
    buffer = clCreateImage2DReal (context, flags, image_format, image_width, image_height, image_depth, image_row_pitch, image_slice_pitch, host_ptr, errcode_ret);
    item = stat_item_new (buffer);
    item->size = image_width * image_height * image_depth; /* FIXME: compute correct size */

    G_LOCK (stat_data);
    g_hash_table_insert (stat_data_state.buffers, buffer, item);
    G_UNLOCK (stat_data);

    return buffer;
}

cl_int
clRetainMemObject (cl_mem memobj)
{
    cl_int ret;
    cl_int (* clRetainMemObjectReal) (cl_mem);
    StatItem *item;

    clRetainMemObjectReal = get_func ("clRetainMemObject");
    ret = clRetainMemObjectReal (memobj);

    G_LOCK (stat_data);

    item = g_hash_table_lookup (stat_data_state.buffers, memobj);

    if (item != NULL) {
        item->refs++;
    }
    else {
        g_print ("clRetainMemObject: unknown buffer object\n");
        dump_trace ();
    }

    G_UNLOCK (stat_data);

    return ret;
}

cl_int
clReleaseMemObject (cl_mem memobj)
{
    cl_int ret;
    cl_int (* clReleaseMemObjectReal) (cl_mem);
    StatItem *item;

    clReleaseMemObjectReal = get_func ("clReleaseMemObject");
    ret = clReleaseMemObjectReal (memobj);

    G_LOCK (stat_data);

    item = g_hash_table_lookup (stat_data_state.buffers, memobj);

    if (item != NULL) {
        item->refs--;
    }
    else {
        g_print ("clReleaseMemObject: unknown buffer object %p\n", memobj);
        dump_trace ();
    }

    G_UNLOCK (stat_data);

    return ret;
}

cl_sampler
clCreateSampler (cl_context context,
                 cl_bool normalized_coords,
                 cl_addressing_mode addressing_mode,
                 cl_filter_mode filter_mode,
                 cl_int *errcode_ret)
{
    cl_sampler sampler;
    StatItem *item;
    cl_sampler (* clCreateSamplerReal) (cl_context, cl_bool, cl_addressing_mode, cl_filter_mode, cl_int *);

    clCreateSamplerReal = get_func ("clCreateSampler");
    sampler = clCreateSamplerReal (context, normalized_coords, addressing_mode, filter_mode, errcode_ret);
    item = stat_item_new (sampler);

    G_LOCK (stat_data);
    g_hash_table_insert (stat_data_state.samplers, sampler, item);
    G_UNLOCK (stat_data);

    return sampler;
}

cl_int
clRetainSampler (cl_sampler sampler)
{
    cl_int ret;
    cl_int (* clRetainSamplerReal) (cl_sampler);
    StatItem *item;

    clRetainSamplerReal = get_func ("clRetainSampler");
    ret = clRetainSamplerReal (sampler);

    G_LOCK (stat_data);

    item = g_hash_table_lookup (stat_data_state.samplers, sampler);

    if (item != NULL) {
        item->refs++;
    }
    else {
        g_print ("clRetainSampler: unknown sampler\n");
        dump_trace ();
    }

    G_UNLOCK (stat_data);

    return ret;
}

cl_int
clReleaseSampler (cl_sampler sampler)
{
    cl_int ret;
    cl_int (* clReleaseSamplerReal) (cl_sampler);
    StatItem *item;

    clReleaseSamplerReal = get_func ("clReleaseSampler");
    ret = clReleaseSamplerReal (sampler);

    G_LOCK (stat_data);

    item = g_hash_table_lookup (stat_data_state.samplers, sampler);

    if (item != NULL) {
        item->refs--;
    }
    else {
        g_print ("clReleaseSampler: unknown sampler %p\n", sampler);
        dump_trace ();
    }

    G_UNLOCK (stat_data);

    return ret;
}

cl_kernel
clCreateKernel (cl_program program,
                const char *kernel_name,
                cl_int *errcode_ret)
{
    cl_kernel kernel;
    StatItem *item;
    cl_kernel (* clCreateKernelReal) (cl_program, const char *, cl_int *);

    clCreateKernelReal = get_func ("clCreateKernel");
    kernel = clCreateKernelReal (program, kernel_name, errcode_ret);
    item = stat_item_new (kernel);

    G_LOCK (stat_data);
    g_hash_table_insert (stat_data_state.kernels, kernel, item);
    G_UNLOCK (stat_data);

    return kernel;
}

cl_int
clRetainKernel (cl_kernel kernel)
{
    cl_int ret;
    cl_int (* clRetainKernelReal) (cl_kernel);
    StatItem *item;

    clRetainKernelReal = get_func ("clRetainKernel");
    ret = clRetainKernelReal (kernel);

    G_LOCK (stat_data);

    item = g_hash_table_lookup (stat_data_state.kernels, kernel);

    if (item != NULL) {
        item->refs++;
    }
    else {
        g_print ("clRetainKernel: unknown kernel\n");
        dump_trace ();
    }

    G_UNLOCK (stat_data);

    return ret;
}

cl_int
clReleaseKernel (cl_kernel kernel)
{
    cl_int ret;
    cl_int (* clReleaseKernelReal) (cl_kernel);
    StatItem *item;

    clReleaseKernelReal = get_func ("clReleaseKernel");
    ret = clReleaseKernelReal (kernel);

    G_LOCK (stat_data);

    item = g_hash_table_lookup (stat_data_state.kernels, kernel);

    if (item != NULL) {
        item->refs--;
    }
    else {
        g_print ("clReleaseKernel: unknown kernel %p\n", kernel);
        dump_trace ();
    }

    G_UNLOCK (stat_data);

    return ret;
}
