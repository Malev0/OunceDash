// A small fake Java VM: enough of JNI for cocos2d-x, RobTop's glue code
// and FMOD. Java methods are C functions registered with java_register().
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "port.h"

typedef enum { JO_CLASS = 1, JO_OBJECT, JO_STRING, JO_ARRAY } JKind;

#define JOBJ_MAGIC 0x4a4f424a

typedef struct JObj {
    uint32_t magic;
    JKind kind;
    char *class_name;   // object/class: the class name (slashes)
    char *str;          // string: UTF-8
    char elem;          // array element type
    int length;
    void *data;
    int global_refs;
    bool permanent;     // classes and VM-owned singletons
} JObj;

typedef struct JMethod {
    char *cls, *name, *sig;
    JavaMethodFn fn;
    struct JMethod *next;
} JMethod;

typedef struct JField { char *cls, *name, *sig; } JField;

static Mutex g_jni_lock;
static JMethod *g_methods;     // registered implementations
static JMethod *g_method_ids;  // ids handed to native code
static JObj **g_classes;
static size_t g_class_count, g_class_cap;

// ------------------------------------------------------------------ locals

typedef struct { JObj **items; size_t count, cap; } LocalList;
static __thread LocalList *t_locals;

static void track_local(JObj *o)
{
    if (!t_locals) t_locals = calloc(1, sizeof(LocalList));
    LocalList *l = t_locals;
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 256;
        l->items = realloc(l->items, l->cap * sizeof(JObj *));
    }
    l->items[l->count++] = o;
}

static void free_obj(JObj *o)
{
    o->magic = 0;
    free(o->str);
    free(o->data);
    free(o->class_name);
    free(o);
}

void jni_release_locals(void)
{
    LocalList *l = t_locals;
    if (!l) return;
    for (size_t i = 0; i < l->count; i++) {
        JObj *o = l->items[i];
        if (o && o->magic == JOBJ_MAGIC && !o->permanent && o->global_refs <= 0)
            free_obj(o);
    }
    l->count = 0;
}

static JObj *new_obj(JKind kind, const char *cls)
{
    JObj *o = calloc(1, sizeof(JObj));
    o->magic = JOBJ_MAGIC;
    o->kind = kind;
    o->class_name = cls ? strdup(cls) : NULL;
    track_local(o);
    return o;
}

static inline JObj *J(const void *p)
{
    JObj *o = (JObj *)p;
    return (o && o->magic == JOBJ_MAGIC) ? o : NULL;
}

// ------------------------------------------------------------------ public helpers

jclass jni_find_class(const char *name)
{
    char norm[256];
    snprintf(norm, sizeof(norm), "%s", name);
    for (char *p = norm; *p; p++) if (*p == '.') *p = '/';
    mutexLock(&g_jni_lock);
    for (size_t i = 0; i < g_class_count; i++) {
        if (strcmp(g_classes[i]->class_name, norm) == 0) {
            mutexUnlock(&g_jni_lock);
            return (jclass)g_classes[i];
        }
    }
    JObj *c = calloc(1, sizeof(JObj));
    c->magic = JOBJ_MAGIC;
    c->kind = JO_CLASS;
    c->class_name = strdup(norm);
    c->permanent = true;
    if (g_class_count == g_class_cap) {
        g_class_cap = g_class_cap ? g_class_cap * 2 : 64;
        g_classes = realloc(g_classes, g_class_cap * sizeof(JObj *));
    }
    g_classes[g_class_count++] = c;
    mutexUnlock(&g_jni_lock);
    return (jclass)c;
}

jstring jni_new_string(const char *s)
{
    JObj *o = new_obj(JO_STRING, "java/lang/String");
    o->str = strdup(s ? s : "");
    return (jstring)o;
}

jobject jni_new_object(const char *cls) { return (jobject)new_obj(JO_OBJECT, cls); }

static int elem_size(char t)
{
    switch (t) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'I': case 'F': return 4;
    default: return 8; // J D L
    }
}

static JObj *new_array(char elem, int n, const void *init)
{
    JObj *o = new_obj(JO_ARRAY, NULL);
    o->elem = elem;
    o->length = n;
    o->data = calloc(n ? n : 1, elem_size(elem));
    if (init) memcpy(o->data, init, (size_t)n * elem_size(elem));
    return o;
}

jintArray jni_new_int_array(const jint *d, int n) { return (jintArray)new_array('I', n, d); }
jfloatArray jni_new_float_array(const jfloat *d, int n) { return (jfloatArray)new_array('F', n, d); }
jbyteArray jni_new_byte_array(const void *d, int n) { return (jbyteArray)new_array('B', n, d); }

const char *jni_string_utf(jobject s)
{
    JObj *o = J(s);
    return (o && o->kind == JO_STRING && o->str) ? o->str : "";
}

void java_register(const char *cls, const char *name, const char *sig, JavaMethodFn fn)
{
    JMethod *m = calloc(1, sizeof(*m));
    m->cls = strdup(cls);
    m->name = strdup(name);
    m->sig = strdup(sig ? sig : "*");
    m->fn = fn;
    m->next = g_methods;
    g_methods = m;
}

// ------------------------------------------------------------------ method ids

static JavaMethodFn find_impl(const char *cls, const char *name, const char *sig)
{
    JavaMethodFn wildcard = NULL;
    for (JMethod *m = g_methods; m; m = m->next) {
        if (strcmp(m->name, name)) continue;
        bool sig_ok = !strcmp(m->sig, "*") || !strcmp(m->sig, sig);
        if (!sig_ok) continue;
        if (!strcmp(m->cls, cls)) return m->fn;
        if (!strcmp(m->cls, "*")) wildcard = m->fn;
    }
    return wildcard;
}

static jmethodID get_method(jclass clazz, const char *name, const char *sig)
{
    JObj *c = J(clazz);
    const char *cls = c && c->class_name ? c->class_name : "?";
    mutexLock(&g_jni_lock);
    for (JMethod *m = g_method_ids; m; m = m->next) {
        if (!strcmp(m->cls, cls) && !strcmp(m->name, name) && !strcmp(m->sig, sig)) {
            mutexUnlock(&g_jni_lock);
            return (jmethodID)m;
        }
    }
    JMethod *m = calloc(1, sizeof(*m));
    m->cls = strdup(cls);
    m->name = strdup(name);
    m->sig = strdup(sig);
    m->fn = find_impl(cls, name, sig);
    m->next = g_method_ids;
    g_method_ids = m;
    mutexUnlock(&g_jni_lock);
    if (!m->fn)
        LOG("JNI: no implementation for %s.%s%s (returns default)\n", cls, name, sig);
    return (jmethodID)m;
}

static char ret_type(const char *sig)
{
    const char *p = strchr(sig, ')');
    return p ? p[1] : 'V';
}

// Reads the arguments described by `sig` from a va_list.
static int parse_args(const char *sig, va_list ap, jvalue *out, int max)
{
    int n = 0;
    const char *p = sig + 1;
    while (*p && *p != ')' && n < max) {
        char t = *p;
        bool is_array = false;
        while (*p == '[') { p++; is_array = true; }
        t = is_array ? 'L' : *p;
        if (*p == 'L') { while (*p && *p != ';') p++; t = 'L'; }
        p++;
        switch (t) {
        case 'Z': out[n].z = (jboolean)va_arg(ap, int); break;
        case 'B': out[n].b = (jbyte)va_arg(ap, int); break;
        case 'C': out[n].c = (jchar)va_arg(ap, int); break;
        case 'S': out[n].s = (jshort)va_arg(ap, int); break;
        case 'I': out[n].i = va_arg(ap, jint); break;
        case 'J': out[n].j = va_arg(ap, jlong); break;
        case 'F': out[n].f = (jfloat)va_arg(ap, double); break;
        case 'D': out[n].d = va_arg(ap, double); break;
        default: out[n].l = va_arg(ap, jobject); break;
        }
        n++;
    }
    return n;
}

static jvalue invoke(jobject self, jmethodID id, const jvalue *args)
{
    JMethod *m = (JMethod *)id;
    jvalue r;
    memset(&r, 0, sizeof(r));
    if (!m) return r;
    if (m->fn) {
        if (!perf_on_main()) return m->fn(self, args);
        uint64_t t = time_ns();
        jvalue r = m->fn(self, args);
        perf_add_java(time_ns() - t);
        return r;
    }
    char rt = ret_type(m->sig);
    const char *rp = strchr(m->sig, ')');
    if (rt == 'L' && rp && !strcmp(rp + 1, "Ljava/lang/String;"))
        r.l = jni_new_string("");
    return r;
}

static jvalue invoke_v(jobject self, jmethodID id, va_list ap)
{
    jvalue args[32];
    JMethod *m = (JMethod *)id;
    if (m) parse_args(m->sig, ap, args, 32);
    return invoke(self, id, args);
}

// ------------------------------------------------------------------ the interface

static jint jGetVersion(JNIEnv *e) { (void)e; return JNI_VERSION_1_6; }
static jclass jFindClass(JNIEnv *e, const char *n) { (void)e; return jni_find_class(n); }
static jclass jGetSuperclass(JNIEnv *e, jclass c) { (void)e; (void)c; return jni_find_class("java/lang/Object"); }
static jboolean jIsAssignableFrom(JNIEnv *e, jclass a, jclass b) { (void)e; (void)a; (void)b; return JNI_TRUE; }
static jint jThrow(JNIEnv *e, jthrowable t) { (void)e; (void)t; LOG("JNI: Throw\n"); return 0; }
static jint jThrowNew(JNIEnv *e, jclass c, const char *m) { (void)e; (void)c; LOG("JNI: ThrowNew %s\n", m); return 0; }
static jthrowable jExceptionOccurred(JNIEnv *e) { (void)e; return NULL; }
static void jExceptionDescribe(JNIEnv *e) { (void)e; }
static void jExceptionClear(JNIEnv *e) { (void)e; }
static void jFatalError(JNIEnv *e, const char *m) { (void)e; fatal_error("JNI FatalError: %s", m); }
static jint jPushLocalFrame(JNIEnv *e, jint c) { (void)e; (void)c; return 0; }
static jobject jPopLocalFrame(JNIEnv *e, jobject r) { (void)e; return r; }
static jobject jNewGlobalRef(JNIEnv *e, jobject o) { (void)e; JObj *j = J(o); if (j) j->global_refs++; return o; }
static void jDeleteGlobalRef(JNIEnv *e, jobject o) { (void)e; JObj *j = J(o); if (j && j->global_refs > 0) j->global_refs--; }
static void jDeleteLocalRef(JNIEnv *e, jobject o) { (void)e; (void)o; }
static jboolean jIsSameObject(JNIEnv *e, jobject a, jobject b) { (void)e; return a == b; }
static jobject jNewLocalRef(JNIEnv *e, jobject o) { (void)e; return o; }
static jint jEnsureLocalCapacity(JNIEnv *e, jint c) { (void)e; (void)c; return 0; }
static jobject jAllocObject(JNIEnv *e, jclass c) { (void)e; JObj *k = J(c); return jni_new_object(k ? k->class_name : "?"); }
static jobject jNewObjectV(JNIEnv *e, jclass c, jmethodID m, va_list ap)
{
    jobject o = jAllocObject(e, c);
    invoke_v(o, m, ap);
    return o;
}
static jobject jNewObject(JNIEnv *e, jclass c, jmethodID m, ...)
{
    va_list ap; va_start(ap, m);
    jobject o = jNewObjectV(e, c, m, ap);
    va_end(ap);
    return o;
}
static jobject jNewObjectA(JNIEnv *e, jclass c, jmethodID m, const jvalue *a)
{
    jobject o = jAllocObject(e, c);
    invoke(o, m, a);
    return o;
}
static jclass jGetObjectClass(JNIEnv *e, jobject o)
{
    (void)e;
    JObj *j = J(o);
    if (!j) return jni_find_class("java/lang/Object");
    if (j->kind == JO_CLASS) return jni_find_class("java/lang/Class");
    if (j->kind == JO_STRING) return jni_find_class("java/lang/String");
    return jni_find_class(j->class_name ? j->class_name : "java/lang/Object");
}
static jboolean jIsInstanceOf(JNIEnv *e, jobject o, jclass c) { (void)e; (void)c; return o != NULL; }
static jmethodID jGetMethodID(JNIEnv *e, jclass c, const char *n, const char *s) { (void)e; return get_method(c, n, s); }

// Call<Type>Method{,V,A} for instance, nonvirtual and static calls.
#define CALLS(Type, jtype, field) \
    static jtype jCall##Type##MethodV(JNIEnv *e, jobject o, jmethodID m, va_list ap) \
    { (void)e; return invoke_v(o, m, ap).field; } \
    static jtype jCall##Type##Method(JNIEnv *e, jobject o, jmethodID m, ...) \
    { va_list ap; va_start(ap, m); jtype r = jCall##Type##MethodV(e, o, m, ap); va_end(ap); return r; } \
    static jtype jCall##Type##MethodA(JNIEnv *e, jobject o, jmethodID m, const jvalue *a) \
    { (void)e; return invoke(o, m, a).field; } \
    static jtype jCallNonvirtual##Type##MethodV(JNIEnv *e, jobject o, jclass c, jmethodID m, va_list ap) \
    { (void)e; (void)c; return invoke_v(o, m, ap).field; } \
    static jtype jCallNonvirtual##Type##Method(JNIEnv *e, jobject o, jclass c, jmethodID m, ...) \
    { va_list ap; va_start(ap, m); jtype r = jCallNonvirtual##Type##MethodV(e, o, c, m, ap); va_end(ap); return r; } \
    static jtype jCallNonvirtual##Type##MethodA(JNIEnv *e, jobject o, jclass c, jmethodID m, const jvalue *a) \
    { (void)e; (void)c; return invoke(o, m, a).field; } \
    static jtype jCallStatic##Type##MethodV(JNIEnv *e, jclass c, jmethodID m, va_list ap) \
    { (void)e; return invoke_v(c, m, ap).field; } \
    static jtype jCallStatic##Type##Method(JNIEnv *e, jclass c, jmethodID m, ...) \
    { va_list ap; va_start(ap, m); jtype r = jCallStatic##Type##MethodV(e, c, m, ap); va_end(ap); return r; } \
    static jtype jCallStatic##Type##MethodA(JNIEnv *e, jclass c, jmethodID m, const jvalue *a) \
    { (void)e; return invoke(c, m, a).field; }

CALLS(Object, jobject, l)
CALLS(Boolean, jboolean, z)
CALLS(Byte, jbyte, b)
CALLS(Char, jchar, c)
CALLS(Short, jshort, s)
CALLS(Int, jint, i)
CALLS(Long, jlong, j)
CALLS(Float, jfloat, f)
CALLS(Double, jdouble, d)

static void jCallVoidMethodV(JNIEnv *e, jobject o, jmethodID m, va_list ap) { (void)e; invoke_v(o, m, ap); }
static void jCallVoidMethod(JNIEnv *e, jobject o, jmethodID m, ...) { va_list ap; va_start(ap, m); jCallVoidMethodV(e, o, m, ap); va_end(ap); }
static void jCallVoidMethodA(JNIEnv *e, jobject o, jmethodID m, const jvalue *a) { (void)e; invoke(o, m, a); }
static void jCallNonvirtualVoidMethodV(JNIEnv *e, jobject o, jclass c, jmethodID m, va_list ap) { (void)e; (void)c; invoke_v(o, m, ap); }
static void jCallNonvirtualVoidMethod(JNIEnv *e, jobject o, jclass c, jmethodID m, ...) { va_list ap; va_start(ap, m); jCallNonvirtualVoidMethodV(e, o, c, m, ap); va_end(ap); }
static void jCallNonvirtualVoidMethodA(JNIEnv *e, jobject o, jclass c, jmethodID m, const jvalue *a) { (void)e; (void)c; invoke(o, m, a); }
static void jCallStaticVoidMethodV(JNIEnv *e, jclass c, jmethodID m, va_list ap) { (void)e; invoke_v(c, m, ap); }
static void jCallStaticVoidMethod(JNIEnv *e, jclass c, jmethodID m, ...) { va_list ap; va_start(ap, m); jCallStaticVoidMethodV(e, c, m, ap); va_end(ap); }
static void jCallStaticVoidMethodA(JNIEnv *e, jclass c, jmethodID m, const jvalue *a) { (void)e; invoke(c, m, a); }

static jmethodID jGetStaticMethodID(JNIEnv *e, jclass c, const char *n, const char *s) { (void)e; return get_method(c, n, s); }

// Fields: Build.VERSION.SDK_INT is the only one anything here needs.
static jfieldID get_field(jclass c, const char *n, const char *s)
{
    JObj *k = J(c);
    JField *f = calloc(1, sizeof(*f));
    f->cls = strdup(k && k->class_name ? k->class_name : "?");
    f->name = strdup(n);
    f->sig = strdup(s);
    return (jfieldID)f;
}
static jfieldID jGetFieldID(JNIEnv *e, jclass c, const char *n, const char *s) { (void)e; return get_field(c, n, s); }
static jfieldID jGetStaticFieldID(JNIEnv *e, jclass c, const char *n, const char *s) { (void)e; return get_field(c, n, s); }
static jint field_int(jfieldID id)
{
    JField *f = (JField *)id;
    if (f && !strcmp(f->name, "SDK_INT")) return 30;
    return 0;
}
static jobject jGetObjectField(JNIEnv *e, jobject o, jfieldID f) { (void)e; (void)o; (void)f; return NULL; }
static jboolean jGetBooleanField(JNIEnv *e, jobject o, jfieldID f) { (void)e; (void)o; (void)f; return 0; }
static jint jGetIntField(JNIEnv *e, jobject o, jfieldID f) { (void)e; (void)o; return field_int(f); }
static jlong jGetLongField(JNIEnv *e, jobject o, jfieldID f) { (void)e; (void)o; (void)f; return 0; }
static jfloat jGetFloatField(JNIEnv *e, jobject o, jfieldID f) { (void)e; (void)o; (void)f; return 0; }
static jobject jGetStaticObjectField(JNIEnv *e, jclass c, jfieldID f) { (void)e; (void)c; (void)f; return NULL; }
static jboolean jGetStaticBooleanField(JNIEnv *e, jclass c, jfieldID f) { (void)e; (void)c; (void)f; return 0; }
static jint jGetStaticIntField(JNIEnv *e, jclass c, jfieldID f) { (void)e; (void)c; return field_int(f); }
static jlong jGetStaticLongField(JNIEnv *e, jclass c, jfieldID f) { (void)e; (void)c; (void)f; return 0; }
static void jSetObjectField(JNIEnv *e, jobject o, jfieldID f, jobject v) { (void)e; (void)o; (void)f; (void)v; }
static void jSetIntField(JNIEnv *e, jobject o, jfieldID f, jint v) { (void)e; (void)o; (void)f; (void)v; }

// Strings
static jstring jNewStringUTF(JNIEnv *e, const char *s) { (void)e; return jni_new_string(s); }
static jsize jGetStringUTFLength(JNIEnv *e, jstring s) { (void)e; return (jsize)strlen(jni_string_utf(s)); }
static const char *jGetStringUTFChars(JNIEnv *e, jstring s, jboolean *c) { (void)e; if (c) *c = 0; return jni_string_utf(s); }
static void jReleaseStringUTFChars(JNIEnv *e, jstring s, const char *u) { (void)e; (void)s; (void)u; }

static int utf8_decode(const char **pp)
{
    const uint8_t *p = (const uint8_t *)*pp;
    int c = *p++;
    if (c >= 0xf0) { c = ((c & 7) << 18) | ((p[0] & 0x3f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f); p += 3; }
    else if (c >= 0xe0) { c = ((c & 15) << 12) | ((p[0] & 0x3f) << 6) | (p[1] & 0x3f); p += 2; }
    else if (c >= 0xc0) { c = ((c & 31) << 6) | (p[0] & 0x3f); p += 1; }
    *pp = (const char *)p;
    return c;
}

static int to_utf16(const char *s, jchar *out, int max)
{
    int n = 0;
    while (*s) {
        int c = utf8_decode(&s);
        if (c >= 0x10000) {
            c -= 0x10000;
            if (out && n + 1 < max) { out[n] = 0xd800 | (c >> 10); out[n + 1] = 0xdc00 | (c & 0x3ff); }
            n += 2;
        } else {
            if (out && n < max) out[n] = (jchar)c;
            n++;
        }
    }
    return n;
}

static jstring jNewString(JNIEnv *e, const jchar *u, jsize len)
{
    (void)e;
    char *buf = malloc((size_t)len * 4 + 1), *p = buf;
    for (jsize i = 0; i < len; i++) {
        uint32_t c = u[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < len) { c = 0x10000 + ((c - 0xd800) << 10) + (u[i + 1] - 0xdc00); i++; }
        if (c < 0x80) *p++ = (char)c;
        else if (c < 0x800) { *p++ = 0xc0 | (c >> 6); *p++ = 0x80 | (c & 0x3f); }
        else if (c < 0x10000) { *p++ = 0xe0 | (c >> 12); *p++ = 0x80 | ((c >> 6) & 0x3f); *p++ = 0x80 | (c & 0x3f); }
        else { *p++ = 0xf0 | (c >> 18); *p++ = 0x80 | ((c >> 12) & 0x3f); *p++ = 0x80 | ((c >> 6) & 0x3f); *p++ = 0x80 | (c & 0x3f); }
    }
    *p = 0;
    jstring s = jni_new_string(buf);
    free(buf);
    return s;
}
static jsize jGetStringLength(JNIEnv *e, jstring s) { (void)e; return to_utf16(jni_string_utf(s), NULL, 0); }
static const jchar *jGetStringChars(JNIEnv *e, jstring s, jboolean *c)
{
    (void)e;
    if (c) *c = 1;
    const char *u = jni_string_utf(s);
    int n = to_utf16(u, NULL, 0);
    jchar *out = calloc(n + 1, sizeof(jchar));
    to_utf16(u, out, n);
    return out;
}
static void jReleaseStringChars(JNIEnv *e, jstring s, const jchar *c) { (void)e; (void)s; free((void *)c); }
static void jGetStringRegion(JNIEnv *e, jstring s, jsize start, jsize len, jchar *buf)
{
    (void)e;
    const char *u = jni_string_utf(s);
    int n = to_utf16(u, NULL, 0);
    jchar *tmp = calloc(n + 1, sizeof(jchar));
    to_utf16(u, tmp, n);
    for (jsize i = 0; i < len && start + i < n; i++) buf[i] = tmp[start + i];
    free(tmp);
}
static void jGetStringUTFRegion(JNIEnv *e, jstring s, jsize start, jsize len, char *buf)
{
    (void)e;
    const char *u = jni_string_utf(s);
    size_t ul = strlen(u);
    if ((size_t)start > ul) start = (jsize)ul;
    size_t n = (size_t)len;
    if (start + n > ul) n = ul - start;
    memcpy(buf, u + start, n);
    buf[n] = 0;
}

// Arrays
static jsize jGetArrayLength(JNIEnv *e, jarray a) { (void)e; JObj *o = J(a); return o ? o->length : 0; }
static jobjectArray jNewObjectArray(JNIEnv *e, jsize n, jclass c, jobject init)
{
    (void)e; (void)c;
    JObj *o = new_array('L', n, NULL);
    for (jsize i = 0; i < n; i++) ((jobject *)o->data)[i] = init;
    return (jobjectArray)o;
}
static jobject jGetObjectArrayElement(JNIEnv *e, jobjectArray a, jsize i)
{
    (void)e;
    JObj *o = J(a);
    return (o && i >= 0 && i < o->length) ? ((jobject *)o->data)[i] : NULL;
}
static void jSetObjectArrayElement(JNIEnv *e, jobjectArray a, jsize i, jobject v)
{
    (void)e;
    JObj *o = J(a);
    if (o && i >= 0 && i < o->length) { ((jobject *)o->data)[i] = v; JObj *j = J(v); if (j) j->global_refs++; }
}

#define ARRAYS(Type, jtype, c) \
    static jtype##Array jNew##Type##Array(JNIEnv *e, jsize n) { (void)e; return (jtype##Array)new_array(c, n, NULL); } \
    static jtype *jGet##Type##ArrayElements(JNIEnv *e, jtype##Array a, jboolean *cp) \
    { (void)e; if (cp) *cp = 0; JObj *o = J(a); return o ? (jtype *)o->data : NULL; } \
    static void jRelease##Type##ArrayElements(JNIEnv *e, jtype##Array a, jtype *p, jint m) { (void)e; (void)a; (void)p; (void)m; } \
    static void jGet##Type##ArrayRegion(JNIEnv *e, jtype##Array a, jsize s, jsize n, jtype *buf) \
    { (void)e; JObj *o = J(a); if (o && s >= 0 && s + n <= o->length) memcpy(buf, (jtype *)o->data + s, (size_t)n * sizeof(jtype)); } \
    static void jSet##Type##ArrayRegion(JNIEnv *e, jtype##Array a, jsize s, jsize n, const jtype *buf) \
    { (void)e; JObj *o = J(a); if (o && s >= 0 && s + n <= o->length) memcpy((jtype *)o->data + s, buf, (size_t)n * sizeof(jtype)); }

ARRAYS(Boolean, jboolean, 'Z')
ARRAYS(Byte, jbyte, 'B')
ARRAYS(Char, jchar, 'C')
ARRAYS(Short, jshort, 'S')
ARRAYS(Int, jint, 'I')
ARRAYS(Long, jlong, 'J')
ARRAYS(Float, jfloat, 'F')
ARRAYS(Double, jdouble, 'D')

static void *jGetPrimitiveArrayCritical(JNIEnv *e, jarray a, jboolean *c) { (void)e; if (c) *c = 0; JObj *o = J(a); return o ? o->data : NULL; }
static void jReleasePrimitiveArrayCritical(JNIEnv *e, jarray a, void *p, jint m) { (void)e; (void)a; (void)p; (void)m; }
static const jchar *jGetStringCritical(JNIEnv *e, jstring s, jboolean *c) { return jGetStringChars(e, s, c); }
static void jReleaseStringCritical(JNIEnv *e, jstring s, const jchar *c) { jReleaseStringChars(e, s, c); }

static jint jRegisterNatives(JNIEnv *e, jclass c, const JNINativeMethod *m, jint n)
{
    (void)e;
    JObj *k = J(c);
    for (jint i = 0; i < n; i++)
        LOG("JNI: RegisterNatives %s.%s%s\n", k ? k->class_name : "?", m[i].name, m[i].signature);
    return 0;
}
static jint jUnregisterNatives(JNIEnv *e, jclass c) { (void)e; (void)c; return 0; }
static jint jMonitorEnter(JNIEnv *e, jobject o) { (void)e; (void)o; return 0; }
static jint jMonitorExit(JNIEnv *e, jobject o) { (void)e; (void)o; return 0; }
static jint jGetJavaVM(JNIEnv *e, JavaVM **vm) { (void)e; *vm = g_vm; return 0; }
static jweak jNewWeakGlobalRef(JNIEnv *e, jobject o) { return jNewGlobalRef(e, o); }
static void jDeleteWeakGlobalRef(JNIEnv *e, jweak o) { jDeleteGlobalRef(e, o); }
static jboolean jExceptionCheck(JNIEnv *e) { (void)e; return JNI_FALSE; }
static jobject jNewDirectByteBuffer(JNIEnv *e, void *a, jlong c) { (void)e; (void)a; (void)c; return NULL; }
static void *jGetDirectBufferAddress(JNIEnv *e, jobject b) { (void)e; (void)b; return NULL; }
static jlong jGetDirectBufferCapacity(JNIEnv *e, jobject b) { (void)e; (void)b; return -1; }
static jobjectRefType jGetObjectRefType(JNIEnv *e, jobject o) { (void)e; (void)o; return JNILocalRefType; }

static void jUnimplemented(void)
{
    uintptr_t lr = (uintptr_t)__builtin_return_address(0);
    char buf[256];
    LOG("JNI: unimplemented function called from %s\n", so_describe_address(lr, buf, sizeof(buf)));
}

static struct JNINativeInterface g_iface;
static struct JNIInvokeInterface g_invoke;
static JNIEnv g_env_storage = &g_iface;
static JavaVM g_vm_storage = &g_invoke;
JavaVM *g_vm = &g_vm_storage;
JNIEnv *g_env = &g_env_storage;

static jint vDestroyJavaVM(JavaVM *vm) { (void)vm; return 0; }
static jint vAttachCurrentThread(JavaVM *vm, JNIEnv **env, void *a) { (void)vm; (void)a; *env = g_env; return 0; }
static jint vDetachCurrentThread(JavaVM *vm) { (void)vm; jni_release_locals(); return 0; }
static jint vGetEnv(JavaVM *vm, void **env, jint v) { (void)vm; (void)v; *env = g_env; return JNI_OK; }

void jni_init(void)
{
    mutexInit(&g_jni_lock);
    void **slots = (void **)&g_iface;
    for (size_t i = 0; i < sizeof(g_iface) / sizeof(void *); i++)
        slots[i] = (void *)jUnimplemented;
    g_iface.reserved0 = g_iface.reserved1 = g_iface.reserved2 = g_iface.reserved3 = NULL;

#define S(n) g_iface.n = j##n
    S(GetVersion); S(FindClass); S(GetSuperclass); S(IsAssignableFrom); S(Throw); S(ThrowNew);
    S(ExceptionOccurred); S(ExceptionDescribe); S(ExceptionClear); S(FatalError);
    S(PushLocalFrame); S(PopLocalFrame); S(NewGlobalRef); S(DeleteGlobalRef); S(DeleteLocalRef);
    S(IsSameObject); S(NewLocalRef); S(EnsureLocalCapacity); S(AllocObject); S(NewObject);
    S(NewObjectV); S(NewObjectA); S(GetObjectClass); S(IsInstanceOf); S(GetMethodID);
#define SC(T) S(Call##T##Method); S(Call##T##MethodV); S(Call##T##MethodA); \
    S(CallNonvirtual##T##Method); S(CallNonvirtual##T##MethodV); S(CallNonvirtual##T##MethodA); \
    S(CallStatic##T##Method); S(CallStatic##T##MethodV); S(CallStatic##T##MethodA)
    SC(Object); SC(Boolean); SC(Byte); SC(Char); SC(Short); SC(Int); SC(Long); SC(Float);
    SC(Double); SC(Void);
    S(GetFieldID); S(GetStaticFieldID); S(GetObjectField); S(GetBooleanField); S(GetIntField);
    S(GetLongField); S(GetFloatField); S(GetStaticObjectField); S(GetStaticBooleanField);
    S(GetStaticIntField); S(GetStaticLongField); S(SetObjectField); S(SetIntField);
    S(GetStaticMethodID);
    S(NewString); S(GetStringLength); S(GetStringChars); S(ReleaseStringChars);
    S(NewStringUTF); S(GetStringUTFLength); S(GetStringUTFChars); S(ReleaseStringUTFChars);
    S(GetArrayLength); S(NewObjectArray); S(GetObjectArrayElement); S(SetObjectArrayElement);
#define SA(T) S(New##T##Array); S(Get##T##ArrayElements); S(Release##T##ArrayElements); \
    S(Get##T##ArrayRegion); S(Set##T##ArrayRegion)
    SA(Boolean); SA(Byte); SA(Char); SA(Short); SA(Int); SA(Long); SA(Float); SA(Double);
    S(RegisterNatives); S(UnregisterNatives); S(MonitorEnter); S(MonitorExit); S(GetJavaVM);
    S(GetStringRegion); S(GetStringUTFRegion); S(GetPrimitiveArrayCritical);
    S(ReleasePrimitiveArrayCritical); S(GetStringCritical); S(ReleaseStringCritical);
    S(NewWeakGlobalRef); S(DeleteWeakGlobalRef); S(ExceptionCheck); S(NewDirectByteBuffer);
    S(GetDirectBufferAddress); S(GetDirectBufferCapacity); S(GetObjectRefType);
#undef S

    g_invoke.DestroyJavaVM = vDestroyJavaVM;
    g_invoke.AttachCurrentThread = vAttachCurrentThread;
    g_invoke.DetachCurrentThread = vDetachCurrentThread;
    g_invoke.GetEnv = vGetEnv;
    g_invoke.AttachCurrentThreadAsDaemon = vAttachCurrentThread;
}
