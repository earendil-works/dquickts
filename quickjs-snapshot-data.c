/*
 * QuickJS heap snapshots: data classes
 *
 * Map, Set, WeakMap, WeakSet and their iterators, WeakRef,
 * FinalizationRegistry, ArrayBuffer, SharedArrayBuffer, typed arrays,
 * DataView and DOMException. Included by quickjs-snapshot.c.
 *
 * Every payload starts with a presence byte (0: the object has no
 * internal state, e.g. its constructor failed half-way).
 *
 * Map, Set, WeakMap, WeakSet: record count, then the key (and the value
 *   for maps) of each record in insertion order. Deleted records that are
 *   only kept for iterators and weak records whose key is dead are not
 *   written. The reader inserts the records with map_add_record(), which
 *   rebuilds the hash table (object and symbol keys hash by address) and
 *   the weak key bookkeeping.
 *
 * Map and Set iterators: the map (undefined once done), the kind and the
 *   position: 0 before the first next(), otherwise 1 + the index of the
 *   last returned record among the written records. When that record was
 *   deleted since, the nearest record before it that is still present is
 *   used instead (0 if none): records are only appended, so next()
 *   continues at the same place. Resolved by a fixup once every map body
 *   is read.
 *
 * WeakRef: the target, or DEAD if it was collected. A dead target is
 *   restored as undefined, which deref() also reports as undefined.
 *
 * FinalizationRegistry: realm, cleanup callback, then each entry: target,
 *   held value, unregister token. A dead target is restored as a dead
 *   object structure, as the engine keeps it, so that the next GC still
 *   enqueues the cleanup job; a dead token becomes undefined, as
 *   finrec_delete_weakref() does.
 *
 * ArrayBuffer, SharedArrayBuffer: flags (detached, resizable, immutable),
 *   byte_length, max_byte_length, then the bytes. The data is always
 *   restored in engine allocated memory (free_func = js_array_buffer_free):
 *   buffers created by the host with JS_NewArrayBuffer() are copied and
 *   their free function and opaque are dropped. SharedArrayBuffers are
 *   only supported when their memory is private to the runtime: no host
 *   allocator (JS_SetSharedArrayBufferFunctions()) and engine allocated
 *   data. Otherwise another thread may share the memory and the write
 *   fails with a TypeError.
 *
 * Typed arrays, DataView: buffer, byte offset, byte length, track_rab
 *   and, for typed arrays, the element count and whether the data pointer
 *   is set. The link into the buffer's array_list and the data pointer
 *   are restored by a fixup because the buffer body may be read later.
 *
 * DOMException: name, message and 1 + the cached legacy code (0: not
 *   computed yet).
 *
 * Limitation: weak containers are appended to rt->weakref_list in cell
 * order, not in creation order. This is only observable as the relative
 * order of cleanup jobs of different FinalizationRegistry objects queued
 * by the same GC.
 */

/* ArrayBuffer flags */
#define SNAP_ABUF_DETACHED  (1 << 0)
#define SNAP_ABUF_RESIZABLE (1 << 1)
#define SNAP_ABUF_IMMUTABLE (1 << 2)

/* fixup of a typed array, DataView or map iterator */
typedef struct {
    JSObject *obj;
    uint32_t count; /* typed array element count, iterator record index */
    BOOL has_ptr; /* typed array data pointer set */
} SnapDataFixup;

static int snap_add_data_natives(JSRuntime *rt, SnapNatives *n)
{
    /* FinalizationRegistry cleanup jobs */
    return snap_natives_push(rt, n, js_finrec_job);
}

static BOOL snap_data_is_view(JSClassID class_id)
{
    return class_id >= JS_CLASS_UINT8C_ARRAY && class_id <= JS_CLASS_DATAVIEW;
}

static int snap_data_add_fixup(SnapReader *r, SnapFixupFunc *func,
                               JSObject *p, uint32_t count, BOOL has_ptr)
{
    SnapDataFixup *f = js_malloc(r->ctx, sizeof(*f));
    if (!f)
        return -1;
    f->obj = p;
    f->count = count;
    f->has_ptr = has_ptr;
    return snap_add_fixup(r, func, f);
}

/*******************************************************************/
/* Map, Set, WeakMap, WeakSet */

static BOOL snap_data_record_is_written(JSMapState *s, JSMapRecord *mr)
{
    return !mr->empty && (!s->is_weak || js_weakref_is_live(mr->key));
}

static int snap_data_write_map(SnapWriter *w, JSObject *p)
{
    JSMapState *s = p->u.map_state;
    BOOL is_set = (p->class_id == JS_CLASS_SET || p->class_id == JS_CLASS_WEAKSET);
    struct list_head *el;
    JSMapRecord *mr;
    uint32_t n;

    bc_put_u8(&w->bc, s != NULL);
    if (!s)
        return 0;
    n = 0;
    list_for_each(el, &s->records) {
        mr = list_entry(el, JSMapRecord, link);
        if (snap_data_record_is_written(s, mr))
            n++;
    }
    bc_put_leb128(&w->bc, n);
    list_for_each(el, &s->records) {
        mr = list_entry(el, JSMapRecord, link);
        if (!snap_data_record_is_written(s, mr))
            continue;
        if (snap_put_value(w, mr->key))
            return -1;
        if (!is_set && snap_put_value(w, mr->value))
            return -1;
    }
    return 0;
}

static int snap_data_read_map(SnapReader *r, JSObject *p)
{
    JSContext *ctx = r->ctx;
    BOOL is_set = (p->class_id == JS_CLASS_SET || p->class_id == JS_CLASS_WEAKSET);
    BOOL is_weak = (p->class_id == JS_CLASS_WEAKMAP || p->class_id == JS_CLASS_WEAKSET);
    JSMapState *s;
    JSMapRecord *mr;
    JSValue key, value;
    uint32_t i, n;
    uint8_t present;

    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (!present)
        return 0;
    if (snap_get_leb(r, &n))
        return -1;
    /* same initial state as js_map_constructor() */
    s = js_mallocz(ctx, sizeof(*s));
    if (!s)
        return -1;
    init_list_head(&s->records);
    s->is_weak = is_weak;
    if (is_weak) {
        s->weakref_header.weakref_type = JS_WEAKREF_TYPE_MAP;
        list_add_tail(&s->weakref_header.link, &r->rt->weakref_list);
    }
    p->u.map_state = s;
    snap_payload_ready(r); /* js_map_finalizer() accepts a NULL hash table */
    s->hash_bits = 1;
    s->hash_size = 1U << s->hash_bits;
    s->hash_table = js_mallocz(ctx, sizeof(s->hash_table[0]) * s->hash_size);
    if (!s->hash_table)
        return -1;
    s->record_count_threshold = 4;

    for(i = 0; i < n; i++) {
        if (snap_get_value(r, &key))
            return -1;
        if (is_weak && !js_weakref_is_target(key)) {
            JS_FreeValue(ctx, key);
            return snap_error(r, "invalid weak collection key");
        }
        value = JS_UNDEFINED;
        if (!is_set && snap_get_value(r, &value)) {
            JS_FreeValue(ctx, key);
            return -1;
        }
        mr = map_add_record(ctx, s, key);
        JS_FreeValue(ctx, key);
        if (!mr) {
            JS_FreeValue(ctx, value);
            return -1;
        }
        mr->value = value;
    }
    return 0;
}

/*******************************************************************/
/* Map and Set iterators */

static int snap_data_write_map_iterator(SnapWriter *w, JSObject *p)
{
    JSMapIteratorData *it = p->u.map_iterator_data;
    uint32_t pos;

    bc_put_u8(&w->bc, it != NULL);
    if (!it)
        return 0;
    if (snap_put_value(w, it->obj))
        return -1;
    bc_put_leb128(&w->bc, it->kind);
    pos = 0;
    if (it->cur_record) {
        JSMapState *s = JS_VALUE_GET_OBJ(it->obj)->u.map_state;
        struct list_head *el, *el1;

        /* nearest written record at or before the current one */
        for(el = &it->cur_record->link; el != &s->records; el = el->prev) {
            if (snap_data_record_is_written(s, list_entry(el, JSMapRecord, link)))
                break;
        }
        if (el != &s->records) {
            pos = 1;
            for(el1 = s->records.next; el1 != el; el1 = el1->next) {
                if (snap_data_record_is_written(s, list_entry(el1, JSMapRecord, link)))
                    pos++;
            }
        }
    }
    bc_put_leb128(&w->bc, pos);
    return 0;
}

static int snap_data_map_iterator_fixup(SnapReader *r, void *opaque, BOOL abort)
{
    SnapDataFixup f = *(SnapDataFixup *)opaque;
    JSMapIteratorData *it;
    JSMapState *s;
    struct list_head *el;
    uint32_t idx;

    js_free_rt(r->rt, opaque);
    if (abort)
        return 0;
    it = f.obj->u.map_iterator_data;
    s = JS_VALUE_GET_OBJ(it->obj)->u.map_state;
    if (!s)
        return snap_error(r, "invalid map iterator");
    idx = f.count;
    list_for_each(el, &s->records) {
        if (idx-- == 0) {
            /* lock the record as js_map_iterator_next() does */
            JSMapRecord *mr = list_entry(el, JSMapRecord, link);
            mr->ref_count++;
            it->cur_record = mr;
            return 0;
        }
    }
    return snap_error(r, "invalid map iterator position");
}

static int snap_data_read_map_iterator(SnapReader *r, JSObject *p)
{
    JSContext *ctx = r->ctx;
    JSClassID map_class_id = JS_CLASS_MAP + (p->class_id - JS_CLASS_MAP_ITERATOR);
    JSMapIteratorData *it;
    uint32_t kind, pos;
    uint8_t present;

    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (!present)
        return 0;
    it = js_malloc(ctx, sizeof(*it));
    if (!it)
        return -1;
    it->obj = JS_UNDEFINED;
    it->kind = JS_ITERATOR_KIND_KEY;
    it->cur_record = NULL; /* set by the fixup */
    p->u.map_iterator_data = it;
    snap_payload_ready(r);
    if (snap_get_value(r, &it->obj) ||
        snap_get_leb(r, &kind) ||
        snap_get_leb(r, &pos))
        return -1;
    if (kind > JS_ITERATOR_KIND_KEY_AND_VALUE ||
        (JS_IsUndefined(it->obj) ? pos != 0 :
         JS_VALUE_GET_TAG(it->obj) != JS_TAG_OBJECT ||
         JS_VALUE_GET_OBJ(it->obj)->class_id != map_class_id))
        return snap_error(r, "invalid map iterator");
    it->kind = kind;
    if (pos != 0)
        return snap_data_add_fixup(r, snap_data_map_iterator_fixup, p, pos - 1, FALSE);
    return 0;
}

/*******************************************************************/
/* WeakRef */

static int snap_data_write_weakref(SnapWriter *w, JSObject *p)
{
    JSWeakRefData *wrd = p->u.opaque;

    bc_put_u8(&w->bc, wrd != NULL);
    if (!wrd)
        return 0;
    return snap_put_weak_value(w, wrd->target);
}

static int snap_data_read_weakref(SnapReader *r, JSObject *p)
{
    JSContext *ctx = r->ctx;
    JSWeakRefData *wrd;
    JSValue target;
    BOOL dead;
    uint8_t present;

    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (!present)
        return 0;
    /* same bookkeeping as js_weakref_constructor() */
    wrd = js_mallocz(ctx, sizeof(*wrd));
    if (!wrd)
        return -1;
    wrd->target = JS_UNDEFINED;
    wrd->weakref_header.weakref_type = JS_WEAKREF_TYPE_WEAKREF;
    list_add_tail(&wrd->weakref_header.link, &r->rt->weakref_list);
    p->u.opaque = wrd;
    snap_payload_ready(r);
    if (snap_get_weak_value(r, &target, &dead))
        return -1;
    /* undefined: dead target, or target already cleared by the GC */
    if (!JS_IsUndefined(target) && !js_weakref_is_target(target)) {
        JS_FreeValue(ctx, target);
        return snap_error(r, "invalid WeakRef target");
    }
    wrd->target = js_weakref_new(ctx, target);
    JS_FreeValue(ctx, target);
    return 0;
}

/*******************************************************************/
/* FinalizationRegistry */

static int snap_data_write_finrec(SnapWriter *w, JSObject *p)
{
    JSFinalizationRegistryData *frd = p->u.opaque;
    struct list_head *el;
    uint32_t n;

    bc_put_u8(&w->bc, frd != NULL);
    if (!frd)
        return 0;
    if (snap_put_ref(w, SNAP_CELL_CONTEXT, frd->realm) ||
        snap_put_value(w, frd->cb))
        return -1;
    n = 0;
    list_for_each(el, &frd->entries)
        n++;
    bc_put_leb128(&w->bc, n);
    list_for_each(el, &frd->entries) {
        JSFinRecEntry *fre = list_entry(el, JSFinRecEntry, link);
        if (snap_put_weak_value(w, fre->target) ||
            snap_put_value(w, fre->held_val) ||
            snap_put_weak_value(w, fre->token))
            return -1;
    }
    return 0;
}

/* weak reference to a collected object: the object structure is kept
   (ref_count = 0) until its last weak reference is freed */
static int snap_data_new_dead_target(SnapReader *r, JSValue *pval)
{
    JSValue obj = JS_NewObjectProtoClass(r->ctx, JS_NULL, JS_CLASS_OBJECT);
    if (JS_IsException(obj))
        return -1;
    *pval = js_weakref_new(r->ctx, obj);
    JS_FreeValue(r->ctx, obj);
    return 0;
}

/* read a weak target, token or key and return its weak reference */
static int snap_data_get_weakref(SnapReader *r, JSValue *pval, BOOL is_target)
{
    JSValue val;
    BOOL dead;

    *pval = JS_UNDEFINED;
    if (snap_get_weak_value(r, &val, &dead))
        return -1;
    if (dead) {
        /* a collected token is cleared by finrec_delete_weakref() */
        return is_target ? snap_data_new_dead_target(r, pval) : 0;
    }
    if (is_target ? !js_weakref_is_target(val) :
        !JS_IsUndefined(val) && !js_weakref_is_target(val)) {
        JS_FreeValue(r->ctx, val);
        return snap_error(r, "invalid FinalizationRegistry entry");
    }
    *pval = js_weakref_new(r->ctx, val);
    JS_FreeValue(r->ctx, val);
    return 0;
}

static int snap_data_read_finrec(SnapReader *r, JSObject *p)
{
    JSContext *ctx = r->ctx;
    JSFinalizationRegistryData *frd;
    JSFinRecEntry *fre;
    JSContext *realm;
    uint32_t i, n;
    uint8_t present;

    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (!present)
        return 0;
    /* the finalizer needs the realm */
    if (snap_get_context(r, &realm))
        return -1;
    if (!realm)
        return snap_error(r, "invalid FinalizationRegistry realm");
    frd = js_mallocz(ctx, sizeof(*frd));
    if (!frd) {
        JS_FreeContext(realm);
        return -1;
    }
    /* same bookkeeping as js_finrec_constructor() */
    frd->weakref_header.weakref_type = JS_WEAKREF_TYPE_FINREC;
    list_add_tail(&frd->weakref_header.link, &r->rt->weakref_list);
    init_list_head(&frd->entries);
    frd->realm = realm;
    frd->cb = JS_UNDEFINED;
    p->u.opaque = frd;
    snap_payload_ready(r);

    if (snap_get_value(r, &frd->cb) || snap_get_leb(r, &n))
        return -1;
    for(i = 0; i < n; i++) {
        fre = js_malloc(ctx, sizeof(*fre));
        if (!fre)
            return -1;
        fre->target = JS_UNDEFINED;
        fre->held_val = JS_UNDEFINED;
        fre->token = JS_UNDEFINED;
        list_add_tail(&fre->link, &frd->entries);
        if (snap_data_get_weakref(r, &fre->target, TRUE) ||
            snap_get_value(r, &fre->held_val) ||
            snap_data_get_weakref(r, &fre->token, FALSE))
            return -1;
    }
    return 0;
}

/*******************************************************************/
/* ArrayBuffer, SharedArrayBuffer */

static int snap_data_write_array_buffer(SnapWriter *w, JSObject *p)
{
    JSArrayBuffer *abuf = p->u.array_buffer;
    const JSSharedArrayBufferFunctions *sf = &w->rt->sab_funcs;
    int flags;

    bc_put_u8(&w->bc, abuf != NULL);
    if (!abuf)
        return 0;
    if (abuf->shared &&
        (sf->sab_alloc || sf->sab_free || sf->sab_dup ||
         abuf->free_func != js_array_buffer_free)) {
        JS_ThrowTypeError(w->ctx, "snapshot: SharedArrayBuffer memory may be shared with another thread");
        return -1;
    }
    flags = 0;
    if (abuf->detached)
        flags |= SNAP_ABUF_DETACHED;
    if (array_buffer_is_resizable(abuf))
        flags |= SNAP_ABUF_RESIZABLE;
    if (abuf->immutable)
        flags |= SNAP_ABUF_IMMUTABLE;
    bc_put_u8(&w->bc, flags);
    bc_put_leb128(&w->bc, abuf->byte_length);
    if (flags & SNAP_ABUF_RESIZABLE)
        bc_put_leb128(&w->bc, abuf->max_byte_length);
    if (!abuf->detached && abuf->byte_length > 0)
        dbuf_put(&w->bc.dbuf, abuf->data, abuf->byte_length);
    return 0;
}

static int snap_data_read_array_buffer(SnapReader *r, JSObject *p)
{
    JSContext *ctx = r->ctx;
    JSRuntime *rt = r->rt;
    BOOL shared = (p->class_id == JS_CLASS_SHARED_ARRAY_BUFFER);
    JSArrayBuffer *abuf;
    uint32_t len, max_len, alloc_len;
    uint8_t present, flags;

    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (!present)
        return 0;
    if (bc_get_u8(&r->bc, &flags) || snap_get_leb(r, &len))
        return -1;
    max_len = len;
    if ((flags & SNAP_ABUF_RESIZABLE) && snap_get_leb(r, &max_len))
        return -1;
    if ((flags & ~(SNAP_ABUF_DETACHED | SNAP_ABUF_RESIZABLE | SNAP_ABUF_IMMUTABLE)) ||
        len > INT32_MAX || max_len > INT32_MAX || max_len < len ||
        ((flags & SNAP_ABUF_DETACHED) && (shared || len != 0)) ||
        ((flags & SNAP_ABUF_IMMUTABLE) && shared))
        return snap_error(r, "invalid ArrayBuffer");
    if (!(flags & SNAP_ABUF_DETACHED) && len > r->bc.buf_end - r->bc.ptr)
        return snap_error(r, "truncated snapshot");

    /* same layout as js_array_buffer_constructor3() for engine
       allocated memory */
    abuf = js_malloc(ctx, sizeof(*abuf));
    if (!abuf)
        return -1;
    abuf->byte_length = len;
    abuf->max_byte_length = (flags & SNAP_ABUF_RESIZABLE) ? max_len : -1;
    abuf->detached = (flags & SNAP_ABUF_DETACHED) != 0;
    abuf->shared = shared;
    abuf->immutable = (flags & SNAP_ABUF_IMMUTABLE) != 0;
    abuf->data = NULL;
    init_list_head(&abuf->array_list);
    abuf->opaque = NULL;
    abuf->free_func = js_array_buffer_free;
    if (abuf->detached)
        goto done;
    if (shared) {
        /* a growable SharedArrayBuffer is never reallocated: allocate
           max_byte_length bytes upfront */
        alloc_len = max_len;
        if (rt->sab_funcs.sab_alloc) {
            abuf->data = rt->sab_funcs.sab_alloc(rt->sab_funcs.sab_opaque,
                                                 max_int(alloc_len, 1));
            if (!abuf->data)
                JS_ThrowOutOfMemory(ctx);
            else
                memset(abuf->data, 0, alloc_len);
        } else {
            abuf->data = js_mallocz(ctx, max_int(alloc_len, 1));
        }
    } else {
        abuf->data = js_malloc(ctx, max_int(len, 1));
    }
    if (!abuf->data) {
        js_free(ctx, abuf);
        return -1;
    }
 done:
    p->u.array_buffer = abuf;
    snap_payload_ready(r);
    return bc_get_buf(&r->bc, abuf->data, len);
}

/*******************************************************************/
/* typed arrays, DataView */

static int snap_data_write_view(SnapWriter *w, JSObject *p)
{
    JSTypedArray *ta = p->u.typed_array;

    bc_put_u8(&w->bc, ta != NULL);
    if (!ta)
        return 0;
    if (snap_put_obj(w, ta->buffer))
        return -1;
    bc_put_leb128(&w->bc, ta->offset);
    bc_put_leb128(&w->bc, ta->length);
    bc_put_u8(&w->bc, ta->track_rab != 0);
    if (p->class_id != JS_CLASS_DATAVIEW) {
        void *ptr = p->u.array.u.ptr;
        if (ptr && ptr != ta->buffer->u.array_buffer->data + ta->offset) {
            JS_ThrowInternalError(w->ctx, "snapshot: inconsistent typed array");
            return -1;
        }
        bc_put_leb128(&w->bc, p->u.array.count);
        bc_put_u8(&w->bc, ptr != NULL);
    }
    return 0;
}

/* link the view to its buffer, which is now read */
static int snap_data_view_fixup(SnapReader *r, void *opaque, BOOL abort)
{
    SnapDataFixup f = *(SnapDataFixup *)opaque;
    JSObject *p = f.obj;
    JSTypedArray *ta;
    JSArrayBuffer *abuf;

    js_free_rt(r->rt, opaque);
    if (abort)
        return 0;
    ta = p->u.typed_array;
    abuf = ta->buffer->u.array_buffer;
    if (!abuf)
        return snap_error(r, "invalid typed array buffer");
    if (f.has_ptr) {
        int size_log2 = typed_array_size_log2(p->class_id);
        if (!abuf->data ||
            (uint64_t)ta->offset + ((uint64_t)f.count << size_log2) > abuf->byte_length)
            return snap_error(r, "invalid typed array bounds");
        p->u.array.u.ptr = abuf->data + ta->offset;
        p->u.array.count = f.count;
    }
    list_add_tail(&ta->link, &abuf->array_list);
    return 0;
}

static int snap_data_read_view(SnapReader *r, JSObject *p)
{
    JSContext *ctx = r->ctx;
    JSObject *pbuf;
    JSTypedArray *ta;
    uint32_t offset, length, count;
    uint8_t present, track_rab, has_ptr;

    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (!present)
        return 0;
    if (snap_get_obj(r, &pbuf) ||
        snap_get_leb(r, &offset) ||
        snap_get_leb(r, &length) ||
        bc_get_u8(&r->bc, &track_rab))
        return -1;
    count = 0;
    has_ptr = FALSE;
    if (p->class_id != JS_CLASS_DATAVIEW &&
        (snap_get_leb(r, &count) || bc_get_u8(&r->bc, &has_ptr)))
        return -1;
    if (!pbuf ||
        (pbuf->class_id != JS_CLASS_ARRAY_BUFFER &&
         pbuf->class_id != JS_CLASS_SHARED_ARRAY_BUFFER) ||
        track_rab > 1 || has_ptr > 1 || count > INT32_MAX ||
        (!has_ptr && count != 0))
        return snap_error(r, "invalid typed array");
    /* same fields as typed_array_init() and js_dataview_constructor();
       the link and the data pointer are set by the fixup */
    ta = js_malloc(ctx, sizeof(*ta));
    if (!ta)
        return -1;
    ta->link.prev = NULL;
    ta->link.next = NULL;
    ta->obj = p;
    ta->buffer = pbuf;
    JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, pbuf));
    ta->offset = offset;
    ta->length = length;
    ta->track_rab = track_rab;
    p->u.typed_array = ta;
    snap_payload_ready(r); /* the finalizer only unlinks a linked view */
    return snap_data_add_fixup(r, snap_data_view_fixup, p, count, has_ptr);
}

/*******************************************************************/
/* DOMException */

static int snap_data_write_domexception(SnapWriter *w, JSObject *p)
{
    JSDOMExceptionData *s = p->u.opaque;

    bc_put_u8(&w->bc, s != NULL);
    if (!s)
        return 0;
    if (snap_put_value(w, s->name) ||
        snap_put_value(w, s->message))
        return -1;
    bc_put_leb128(&w->bc, s->code + 1);
    return 0;
}

static int snap_data_read_domexception(SnapReader *r, JSObject *p)
{
    JSDOMExceptionData *s;
    uint32_t code;
    uint8_t present;

    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (!present)
        return 0;
    s = js_malloc(r->ctx, sizeof(*s));
    if (!s)
        return -1;
    s->name = JS_UNDEFINED;
    s->message = JS_UNDEFINED;
    s->code = -1;
    p->u.opaque = s;
    snap_payload_ready(r);
    if (snap_get_value(r, &s->name) ||
        snap_get_value(r, &s->message) ||
        snap_get_leb(r, &code))
        return -1;
    if (!JS_IsString(s->name) || !JS_IsString(s->message) ||
        code > countof(js_dom_exception_names) + 1)
        return snap_error(r, "invalid DOMException");
    s->code = (int)code - 1;
    return 0;
}

/*******************************************************************/

static int snap_write_data_payload(SnapWriter *w, JSObject *p)
{
    switch(p->class_id) {
    case JS_CLASS_MAP:
    case JS_CLASS_SET:
    case JS_CLASS_WEAKMAP:
    case JS_CLASS_WEAKSET:
        return snap_data_write_map(w, p);
    case JS_CLASS_MAP_ITERATOR:
    case JS_CLASS_SET_ITERATOR:
        return snap_data_write_map_iterator(w, p);
    case JS_CLASS_WEAK_REF:
        return snap_data_write_weakref(w, p);
    case JS_CLASS_FINALIZATION_REGISTRY:
        return snap_data_write_finrec(w, p);
    case JS_CLASS_DOM_EXCEPTION:
        return snap_data_write_domexception(w, p);
    case JS_CLASS_ARRAY_BUFFER:
    case JS_CLASS_SHARED_ARRAY_BUFFER:
        return snap_data_write_array_buffer(w, p);
    default:
        if (snap_data_is_view(p->class_id))
            return snap_data_write_view(w, p);
        return 1;
    }
}

static int snap_read_data_payload(SnapReader *r, JSObject *p)
{
    switch(p->class_id) {
    case JS_CLASS_MAP:
    case JS_CLASS_SET:
    case JS_CLASS_WEAKMAP:
    case JS_CLASS_WEAKSET:
        return snap_data_read_map(r, p);
    case JS_CLASS_MAP_ITERATOR:
    case JS_CLASS_SET_ITERATOR:
        return snap_data_read_map_iterator(r, p);
    case JS_CLASS_WEAK_REF:
        return snap_data_read_weakref(r, p);
    case JS_CLASS_FINALIZATION_REGISTRY:
        return snap_data_read_finrec(r, p);
    case JS_CLASS_DOM_EXCEPTION:
        return snap_data_read_domexception(r, p);
    case JS_CLASS_ARRAY_BUFFER:
    case JS_CLASS_SHARED_ARRAY_BUFFER:
        return snap_data_read_array_buffer(r, p);
    default:
        if (snap_data_is_view(p->class_id))
            return snap_data_read_view(r, p);
        return 1;
    }
}
