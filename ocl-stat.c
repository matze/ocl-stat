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
dump_info (void)
{
    GList *items;
    GList *items_alive;

    G_LOCK (stat_data);

    items = g_hash_table_get_values (stat_data_state.contexts);
    items_alive = get_alive_items (items);

    g_print ("Contexts created: %i\n", g_list_length (items));
    g_print ("Contexts alive: %i\n", g_list_length (items_alive));

    g_list_free (items);
    g_list_free (items_alive);

    items = g_hash_table_get_values (stat_data_state.buffers);
    items_alive = get_alive_items (items);

    g_print ("Buffers created: %i\n", g_list_length (items));
    g_print ("Buffers alive: %i\n", g_list_length (items_alive));

    if (g_list_length (items_alive) > 0)
        g_print ("Buffers memory leak: %zu\n", total_buffer_size (items_alive));

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

        if (item->refs == 0) {
            g_hash_table_remove (stat_data_state.contexts, item->obj);
            g_free (item);
        }
    }
    else {
        g_print ("clReleaseContext: unknown context\n");
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
    g_print ("inserting %p\n", buffer);

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

        if (item->refs == 0) {
            g_hash_table_remove (stat_data_state.buffers, item->obj);
            g_free (item);
        }
    }
    else {
        g_print ("clReleaseMemObject: unknown buffer object %p\n", memobj);
        dump_trace ();
    }

    G_UNLOCK (stat_data);

    return ret;
}
