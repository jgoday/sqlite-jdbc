/*
 * Copyright (c) 2007 David Crawshaw <david@zentus.com>
 * 
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "NativeDB.h"
#include "sqlite3.h"

static jclass dbclass = 0;
static jclass  fclass = 0;
static jclass  aclass = 0;
static jclass pclass = 0;

static void * toref(jlong value)
{
    jvalue ret;
    ret.j = value;
    return (void *) ret.l;
}

static jlong fromref(void * value)
{
    jvalue ret;
    ret.l = value;
    return ret.j;
}

static void throwex(JNIEnv *env, jobject this)
{
    static jmethodID mth_throwex = 0;

    if (!mth_throwex)
        mth_throwex = (*env)->GetMethodID(env, dbclass, "throwex", "()V");

    (*env)->CallVoidMethod(env, this, mth_throwex);
}

static void throwex_errorcode(JNIEnv *env, jobject this, int errorCode)
{
    static jmethodID mth_throwex = 0;

    if (!mth_throwex)
        mth_throwex = (*env)->GetMethodID(env, dbclass, "throwex", "(I)V");

    (*env)->CallVoidMethod(env, this, mth_throwex, (jint) errorCode);
}

static void throwex_msg(JNIEnv *env, const char *str)
{
    static jmethodID mth_throwexmsg = 0;

    if (!mth_throwexmsg) mth_throwexmsg = (*env)->GetStaticMethodID(
            env, dbclass, "throwex", "(Ljava/lang/String;)V");

    (*env)->CallStaticVoidMethod(env, dbclass, mth_throwexmsg,
                                (*env)->NewStringUTF(env, str));
}

static void throwex_outofmemory(JNIEnv *env)
{
    throwex_msg(env, "Out of memory");
}

static jbyteArray stringToUtf8ByteArray(JNIEnv *env, jstring str)
{
    static jmethodID mth_stringToUtf8ByteArray = 0;

    jobject result;

    if (!mth_stringToUtf8ByteArray) mth_stringToUtf8ByteArray = (*env)->GetStaticMethodID(
            env, dbclass, "stringToUtf8ByteArray", "(Ljava/lang/String;)[B");

    result = (*env)->CallStaticObjectMethod(env, dbclass, mth_stringToUtf8ByteArray, str);

    return (jbyteArray) result;
}

static jstring utf8ByteArrayToString(JNIEnv *env, jbyteArray utf8bytes)
{
    static jmethodID mth_utf8ByteArrayToString = 0;

    jobject result;

    if (!mth_utf8ByteArrayToString) mth_utf8ByteArrayToString = (*env)->GetStaticMethodID(
            env, dbclass, "utf8ByteArrayToString", "([B)Ljava/lang/String;");

    result = (*env)->CallStaticObjectMethod(env, dbclass, mth_utf8ByteArrayToString, utf8bytes);

    return (jstring) result;
}

static jstring utf8BytesToString(JNIEnv *env, const char* bytes, int nbytes)
{
    jstring result;
    jbyteArray utf8bytes;

    if (!bytes)
    {
        return NULL;
    }

    utf8bytes = (*env)->NewByteArray(env, (jsize) nbytes);
    if (!utf8bytes)
    {
        throwex_outofmemory(env);
        return NULL;
    }

    (*env)->SetByteArrayRegion(env, utf8bytes, (jsize) 0, (jsize) nbytes, (const jbyte*) bytes);

    result = utf8ByteArrayToString(env, utf8bytes);

    (*env)->DeleteLocalRef(env, utf8bytes);

    return result;
}

static void stringToUtf8Bytes(JNIEnv *env, jstring str, char** bytes, int* nbytes)
{
    jbyteArray utf8bytes;
    jsize utf8bytes_length;
    char* buf;

    *bytes = NULL;
    if (nbytes) *nbytes = 0;

    if (!str)
    {
        return;
    }

    utf8bytes = stringToUtf8ByteArray(env, str);
    if (!utf8bytes)
     {
        return;
    }

    utf8bytes_length = (*env)->GetArrayLength(env, (jarray) utf8bytes);

    buf = (char*) malloc(utf8bytes_length + 1);
    if (!buf)
    {
        throwex_outofmemory(env);
        return;
    }

    (*env)->GetByteArrayRegion(env, utf8bytes, 0, utf8bytes_length, (jbyte*)buf);

    buf[utf8bytes_length] = '\0';

    *bytes = buf;
    if (nbytes) *nbytes = (int) utf8bytes_length;
}

static void freeUtf8Bytes(char* bytes)
{
    if (bytes)
    {
        free(bytes);
    }
}

static sqlite3 * gethandle(JNIEnv *env, jobject this)
{
    static jfieldID pointer = 0;
    if (!pointer) pointer = (*env)->GetFieldID(env, dbclass, "pointer", "J");

    return (sqlite3 *)toref((*env)->GetLongField(env, this, pointer));
}

static void sethandle(JNIEnv *env, jobject this, sqlite3 * ref)
{
    static jfieldID pointer = 0;
    if (!pointer) pointer = (*env)->GetFieldID(env, dbclass, "pointer", "J");

    (*env)->SetLongField(env, this, pointer, fromref(ref));
}


// User Defined Function SUPPORT ////////////////////////////////////

struct UDFData {
    JavaVM *vm;
    jobject func;
    struct UDFData *next;  // linked list of all UDFData instances
};

/* Returns the sqlite3_value for the given arg of the given function.
 * If 0 is returned, an exception has been thrown to report the reason. */
static sqlite3_value * tovalue(JNIEnv *env, jobject function, jint arg)
{
    jlong value_pntr = 0;
    jint numArgs = 0;
    static jfieldID func_value = 0,
                    func_args = 0;

    if (!func_value || !func_args) {
        func_value = (*env)->GetFieldID(env, fclass, "value", "J");
        func_args  = (*env)->GetFieldID(env, fclass, "args", "I");
    }

    // check we have any business being here
    if (arg  < 0) { throwex_msg(env, "negative arg out of range"); return 0; }
    if (!function) { throwex_msg(env, "inconstent function"); return 0; }

    value_pntr = (*env)->GetLongField(env, function, func_value);
    numArgs = (*env)->GetIntField(env, function, func_args);

    if (value_pntr == 0) { throwex_msg(env, "no current value"); return 0; }
    if (arg >= numArgs) { throwex_msg(env, "arg out of range"); return 0; }

    return ((sqlite3_value**)toref(value_pntr))[arg];
}

/* called if an exception occured processing xFunc */
static void xFunc_error(sqlite3_context *context, JNIEnv *env)
{
    jstring msg = 0;
    char *msg_bytes;
    int msg_nbytes;

    jclass exclass = 0;
    static jmethodID exp_msg = 0;
    jthrowable ex = (*env)->ExceptionOccurred(env);

    (*env)->ExceptionClear(env);

    if (!exp_msg) {
        exclass = (*env)->FindClass(env, "java/lang/Throwable");
        exp_msg = (*env)->GetMethodID(
                env, exclass, "toString", "()Ljava/lang/String;");
    }

    msg = (jstring)(*env)->CallObjectMethod(env, ex, exp_msg);
    if (!msg) { sqlite3_result_error(context, "unknown error", 13); return; }

    stringToUtf8Bytes(env, msg, &msg_bytes, &msg_nbytes);
    if (!msg_bytes) { sqlite3_result_error_nomem(context); return; }

    sqlite3_result_error(context, msg_bytes, msg_nbytes);
    freeUtf8Bytes(msg_bytes);
}

/* used to call xFunc, xStep and xFinal */
static void xCall(
    sqlite3_context *context,
    int args,
    sqlite3_value** value,
    jobject func,
    jmethodID method)
{
    static jfieldID fld_context = 0,
                     fld_value = 0,
                     fld_args = 0;
    JNIEnv *env = 0;
    struct UDFData *udf = 0;

    udf = (struct UDFData*)sqlite3_user_data(context);
    assert(udf);
    (*udf->vm)->AttachCurrentThread(udf->vm, (void **)&env, 0);
    if (!func) func = udf->func;

    if (!fld_context || !fld_value || !fld_args) {
        fld_context = (*env)->GetFieldID(env, fclass, "context", "J");
        fld_value   = (*env)->GetFieldID(env, fclass, "value", "J");
        fld_args    = (*env)->GetFieldID(env, fclass, "args", "I");
    }

    (*env)->SetLongField(env, func, fld_context, fromref(context));
    (*env)->SetLongField(env, func, fld_value, value ? fromref(value) : 0);
    (*env)->SetIntField(env, func, fld_args, args);

    (*env)->CallVoidMethod(env, func, method);

    // check if xFunc threw an Exception
    if ((*env)->ExceptionCheck(env)) {
        xFunc_error(context, env);
    }

    (*env)->SetLongField(env, func, fld_context, 0);
    (*env)->SetLongField(env, func, fld_value, 0);
    (*env)->SetIntField(env, func, fld_args, 0);
}


void xFunc(sqlite3_context *context, int args, sqlite3_value** value)
{
    static jmethodID mth = 0;
    if (!mth) {
        JNIEnv *env;
        struct UDFData *udf = (struct UDFData*)sqlite3_user_data(context);
        (*udf->vm)->AttachCurrentThread(udf->vm, (void **)&env, 0);
        mth = (*env)->GetMethodID(env, fclass, "xFunc", "()V");
    }
    xCall(context, args, value, 0, mth);
}

void xStep(sqlite3_context *context, int args, sqlite3_value** value)
{
    JNIEnv *env;
    struct UDFData *udf;
    jobject *func = 0;
    static jmethodID mth = 0;
    static jmethodID clone = 0;

    if (!mth || !clone) {
        udf = (struct UDFData*)sqlite3_user_data(context);
        (*udf->vm)->AttachCurrentThread(udf->vm, (void **)&env, 0);

        mth = (*env)->GetMethodID(env, aclass, "xStep", "()V");
        clone = (*env)->GetMethodID(env, aclass, "clone",
            "()Ljava/lang/Object;");
    }

    // clone the Function.Aggregate instance and store a pointer
    // in SQLite's aggregate_context (clean up in xFinal)
    func = sqlite3_aggregate_context(context, sizeof(jobject));
    if (!*func) {
        udf = (struct UDFData*)sqlite3_user_data(context);
        (*udf->vm)->AttachCurrentThread(udf->vm, (void **)&env, 0);

        *func = (*env)->CallObjectMethod(env, udf->func, clone);
        *func = (*env)->NewGlobalRef(env, *func);
    }

    xCall(context, args, value, *func, mth);
}

void xFinal(sqlite3_context *context)
{
    JNIEnv *env = 0;
    struct UDFData *udf = 0;
    jobject *func = 0;
    static jmethodID mth = 0;

    udf = (struct UDFData*)sqlite3_user_data(context);
    (*udf->vm)->AttachCurrentThread(udf->vm, (void **)&env, 0);

    if (!mth) mth = (*env)->GetMethodID(env, aclass, "xFinal", "()V");

    func = sqlite3_aggregate_context(context, sizeof(jobject));
    assert(*func); // disaster

    xCall(context, 0, 0, *func, mth);

    // clean up Function.Aggregate instance
    (*env)->DeleteGlobalRef(env, *func);
}


// INITIALISATION ///////////////////////////////////////////////////

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    JNIEnv* env = 0;

    if (JNI_OK != (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_2))
        return JNI_ERR;

    dbclass = (*env)->FindClass(env, "org/sqlite/core/NativeDB");
    if (!dbclass) return JNI_ERR;
    dbclass = (*env)->NewGlobalRef(env, dbclass);

    fclass = (*env)->FindClass(env, "org/sqlite/Function");
    if (!fclass) return JNI_ERR;
    fclass = (*env)->NewGlobalRef(env, fclass);

    aclass = (*env)->FindClass(env, "org/sqlite/Function$Aggregate");
    if (!aclass) return JNI_ERR;
    aclass = (*env)->NewGlobalRef(env, aclass);

    pclass = (*env)->FindClass(env, "org/sqlite/core/DB$ProgressObserver");
    if(!pclass) return JNI_ERR;
    pclass = (*env)->NewGlobalRef(env, pclass);

    return JNI_VERSION_1_2;
}


// WRAPPERS for sqlite_* functions //////////////////////////////////

JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_shared_1cache(
        JNIEnv *env, jobject this, jboolean enable)
{
    return sqlite3_enable_shared_cache(enable ? 1 : 0);
}


JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_enable_1load_1extension(
        JNIEnv *env, jobject this, jboolean enable)
{
    return sqlite3_enable_load_extension(gethandle(env, this), enable ? 1 : 0);
}


JNIEXPORT void JNICALL Java_org_sqlite_core_NativeDB__1open(
        JNIEnv *env, jobject this, jstring file, jint flags)
{
    int ret;
    sqlite3 *db = gethandle(env, this);
    char *file_bytes;

    if (db) {
        throwex_msg(env, "DB already open");
        sqlite3_close(db);
        return;
    }

    stringToUtf8Bytes(env, file, &file_bytes, NULL);
    if (!file_bytes) return;

    ret = sqlite3_open_v2(file_bytes, &db, flags, NULL);
    freeUtf8Bytes(file_bytes);

    if (ret != SQLITE_OK) {
        throwex_errorcode(env, this, ret);
        sqlite3_close(db);
        return;
    }

    // Ignore failures, as we can tolerate regular result codes.
    (void) sqlite3_extended_result_codes(db, 1);

    sethandle(env, this, db);
}

JNIEXPORT void JNICALL Java_org_sqlite_core_NativeDB__1close(
        JNIEnv *env, jobject this)
{
    if (sqlite3_close(gethandle(env, this)) != SQLITE_OK)
    {
        throwex(env, this);
    }
    sethandle(env, this, 0);
}

JNIEXPORT void JNICALL Java_org_sqlite_core_NativeDB_interrupt(JNIEnv *env, jobject this)
{
    sqlite3_interrupt(gethandle(env, this));
}

JNIEXPORT void JNICALL Java_org_sqlite_core_NativeDB_busy_1timeout(
    JNIEnv *env, jobject this, jint ms)
{
    sqlite3_busy_timeout(gethandle(env, this), ms);
}

JNIEXPORT jlong JNICALL Java_org_sqlite_core_NativeDB_prepare(
        JNIEnv *env, jobject this, jstring sql)
{
    sqlite3* db = gethandle(env, this);
    sqlite3_stmt* stmt;
    char* sql_bytes;
    int sql_nbytes;
    int status;

    stringToUtf8Bytes(env, sql, &sql_bytes, &sql_nbytes);
    if (!sql_bytes) return fromref(0);

    status = sqlite3_prepare_v2(db, sql_bytes, sql_nbytes, &stmt, 0);
    freeUtf8Bytes(sql_bytes);

    if (status != SQLITE_OK) {
        throwex_errorcode(env, this, status);
        return fromref(0);
    }
    return fromref(stmt);
}


JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB__1exec(
        JNIEnv *env, jobject this, jstring sql)
{
    sqlite3* db = gethandle(env, this);
    char* sql_bytes;
    int status;

    if (!db)
    {
        throwex_errorcode(env, this, SQLITE_MISUSE);
        return SQLITE_MISUSE;
    }

    stringToUtf8Bytes(env, sql, &sql_bytes, NULL);
    if (!sql_bytes)
    {
        return SQLITE_ERROR;
    }

    status = sqlite3_exec(db, sql_bytes, 0, 0, NULL);
    freeUtf8Bytes(sql_bytes);

    if (status != SQLITE_OK) {
        throwex_errorcode(env, this, status);
    }

    return status;
}


JNIEXPORT jstring JNICALL Java_org_sqlite_core_NativeDB_errmsg(JNIEnv *env, jobject this)
{
    const char *str = (const char*) sqlite3_errmsg(gethandle(env, this));
    if (!str) return NULL;
    return utf8BytesToString(env, str, strlen(str));
}

JNIEXPORT jstring JNICALL Java_org_sqlite_core_NativeDB_libversion(
        JNIEnv *env, jobject this)
{
    return (*env)->NewStringUTF(env, sqlite3_libversion());
}

JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_changes(
        JNIEnv *env, jobject this)
{
    return sqlite3_changes(gethandle(env, this));
}

JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_total_1changes(
        JNIEnv *env, jobject this)
{
    return sqlite3_total_changes(gethandle(env, this));
}

JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_finalize(
        JNIEnv *env, jobject this, jlong stmt)
{
    return sqlite3_finalize(toref(stmt));
}

JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_step(
        JNIEnv *env, jobject this, jlong stmt)
{
    return sqlite3_step(toref(stmt));
}

JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_reset(
        JNIEnv *env, jobject this, jlong stmt)
{
    return sqlite3_reset(toref(stmt));
}

JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_clear_1bindings(
        JNIEnv *env, jobject this, jlong stmt)
{
    return sqlite3_clear_bindings(toref(stmt));
}

JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_bind_1parameter_1count(
        JNIEnv *env, jobject this, jlong stmt)
{
    return sqlite3_bind_parameter_count(toref(stmt));
}

JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_column_1count(
        JNIEnv *env, jobject this, jlong stmt)
{
    return sqlite3_column_count(toref(stmt));
}

JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_column_1type(
        JNIEnv *env, jobject this, jlong stmt, jint col)
{
    return sqlite3_column_type(toref(stmt), col);
}

JNIEXPORT jstring JNICALL Java_org_sqlite_core_NativeDB_column_1decltype(
        JNIEnv *env, jobject this, jlong stmt, jint col)
{
    const char *str = (const char*) sqlite3_column_decltype(toref(stmt), col);
    if (!str) return NULL;
    return utf8BytesToString(env, str, strlen(str));
}

JNIEXPORT jstring JNICALL Java_org_sqlite_core_NativeDB_column_1table_1name(
        JNIEnv *env, jobject this, jlong stmt, jint col)
{
    const char *str = sqlite3_column_table_name(toref(stmt), col);
    if (!str) return NULL;
    return utf8BytesToString(env, str, strlen(str));
}

JNIEXPORT jstring JNICALL Java_org_sqlite_core_NativeDB_column_1name(
        JNIEnv *env, jobject this, jlong stmt, jint col)
{
    const char *str = sqlite3_column_name(toref(stmt), col);
    if (!str) return NULL;
    return utf8BytesToString(env, str, strlen(str));
}

JNIEXPORT jstring JNICALL Java_org_sqlite_core_NativeDB_column_1text(
        JNIEnv *env, jobject this, jlong stmt, jint col)
{
    const char *bytes;
    int nbytes;

    bytes = (const char*) sqlite3_column_text(toref(stmt), col);
    nbytes = sqlite3_column_bytes(toref(stmt), col);

    if (!bytes && sqlite3_errcode(gethandle(env, this)) == SQLITE_NOMEM)
    {
        throwex_outofmemory(env);
        return NULL;
    }

    return utf8BytesToString(env, bytes, nbytes);
}

JNIEXPORT jbyteArray JNICALL Java_org_sqlite_core_NativeDB_column_1blob(
        JNIEnv *env, jobject this, jlong stmt, jint col)
{
    int type;
    int length;
    jbyteArray jBlob;
    const void *blob;
 
    // The value returned by sqlite3_column_type() is only meaningful if no type conversions have occurred
    type = sqlite3_column_type(toref(stmt), col);
    blob = sqlite3_column_blob(toref(stmt), col);
    if (!blob && sqlite3_errcode(gethandle(env, this)) == SQLITE_NOMEM)
    {
        throwex_outofmemory(env);
        return NULL;
    }
    if (!blob) {
        if (type == SQLITE_NULL) {
            return NULL;
        }
        else {
            // The return value from sqlite3_column_blob() for a zero-length BLOB is a NULL pointer.
            jBlob = (*env)->NewByteArray(env, 0);
            if (!jBlob) { throwex_outofmemory(env); return 0; }
            return jBlob;
        }
    }

    length = sqlite3_column_bytes(toref(stmt), col);
    jBlob = (*env)->NewByteArray(env, length);
    if (!jBlob) { throwex_outofmemory(env); return 0; }

    (*env)->SetByteArrayRegion(env, jBlob, (jsize) 0, (jsize) length, (const jbyte*) blob);

    return jBlob;
}

JNIEXPORT jdouble JNICALL Java_org_sqlite_core_NativeDB_column_1double(
        JNIEnv *env, jobject this, jlong stmt, jint col)
{
    return sqlite3_column_double(toref(stmt), col);
}

JNIEXPORT jlong JNICALL Java_org_sqlite_core_NativeDB_column_1long(
        JNIEnv *env, jobject this, jlong stmt, jint col)
{
    return sqlite3_column_int64(toref(stmt), col);
}

JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_column_1int(
        JNIEnv *env, jobject this, jlong stmt, jint col)
{
    return sqlite3_column_int(toref(stmt), col);
}

JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_bind_1null(
        JNIEnv *env, jobject this, jlong stmt, jint pos)
{
    return sqlite3_bind_null(toref(stmt), pos);
}

JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_bind_1int(
        JNIEnv *env, jobject this, jlong stmt, jint pos, jint v)
{
    return sqlite3_bind_int(toref(stmt), pos, v);
}

JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_bind_1long(
        JNIEnv *env, jobject this, jlong stmt, jint pos, jlong v)
{
    return sqlite3_bind_int64(toref(stmt), pos, v);
}

JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_bind_1double(
        JNIEnv *env, jobject this, jlong stmt, jint pos, jdouble v)
{
    return sqlite3_bind_double(toref(stmt), pos, v);
}

JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_bind_1text(
        JNIEnv *env, jobject this, jlong stmt, jint pos, jstring v)
{
    int rc;
    char* v_bytes;
    int v_nbytes;

    stringToUtf8Bytes(env, v, &v_bytes, &v_nbytes);
    if (!v_bytes) return SQLITE_ERROR;

    rc = sqlite3_bind_text(toref(stmt), pos, v_bytes, v_nbytes, SQLITE_TRANSIENT);
    freeUtf8Bytes(v_bytes);

    return rc;
}

JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_bind_1blob(
        JNIEnv *env, jobject this, jlong stmt, jint pos, jbyteArray v)
{
    jint rc;
    void *a;
    jsize size = (*env)->GetArrayLength(env, v);
    a = (*env)->GetPrimitiveArrayCritical(env, v, 0);
    if (!a) { throwex_outofmemory(env); return 0; }
    rc = sqlite3_bind_blob(toref(stmt), pos, a, size, SQLITE_TRANSIENT);
    (*env)->ReleasePrimitiveArrayCritical(env, v, a, JNI_ABORT);
    return rc;
}

JNIEXPORT void JNICALL Java_org_sqlite_core_NativeDB_result_1null(
        JNIEnv *env, jobject this, jlong context)
{
    sqlite3_result_null(toref(context));
}

JNIEXPORT void JNICALL Java_org_sqlite_core_NativeDB_result_1text(
        JNIEnv *env, jobject this, jlong context, jstring value)
{
    char* value_bytes;
    int value_nbytes;

    if (value == NULL) { sqlite3_result_null(toref(context)); return; }

    stringToUtf8Bytes(env, value, &value_bytes, &value_nbytes);
    if (!value_bytes)
    {
        sqlite3_result_error_nomem(toref(context));
        return;
    }

    sqlite3_result_text(toref(context), value_bytes, value_nbytes, SQLITE_TRANSIENT);
    freeUtf8Bytes(value_bytes);
}

JNIEXPORT void JNICALL Java_org_sqlite_core_NativeDB_result_1blob(
        JNIEnv *env, jobject this, jlong context, jobject value)
{
    jbyte *bytes;
    jsize size;

    if (value == NULL) { sqlite3_result_null(toref(context)); return; }

    size = (*env)->GetArrayLength(env, value);
    bytes = (*env)->GetPrimitiveArrayCritical(env, value, 0);
    if (!bytes) { throwex_outofmemory(env); return; }
    sqlite3_result_blob(toref(context), bytes, size, SQLITE_TRANSIENT);
    (*env)->ReleasePrimitiveArrayCritical(env, value, bytes, JNI_ABORT);
}

JNIEXPORT void JNICALL Java_org_sqlite_core_NativeDB_result_1double(
        JNIEnv *env, jobject this, jlong context, jdouble value)
{
    sqlite3_result_double(toref(context), value);
}

JNIEXPORT void JNICALL Java_org_sqlite_core_NativeDB_result_1long(
        JNIEnv *env, jobject this, jlong context, jlong value)
{
    sqlite3_result_int64(toref(context), value);
}

JNIEXPORT void JNICALL Java_org_sqlite_core_NativeDB_result_1int(
        JNIEnv *env, jobject this, jlong context, jint value)
{
    sqlite3_result_int(toref(context), value);
}

JNIEXPORT jstring JNICALL Java_org_sqlite_core_NativeDB_value_1text(
        JNIEnv *env, jobject this, jobject f, jint arg)
{
    const char* bytes;
    int nbytes;

    sqlite3_value *value = tovalue(env, f, arg);
    if (!value) return NULL;

    bytes = (const char*) sqlite3_value_text(value);
    nbytes = sqlite3_value_bytes(value);

    return utf8BytesToString(env, bytes, nbytes);
}

JNIEXPORT jbyteArray JNICALL Java_org_sqlite_core_NativeDB_value_1blob(
        JNIEnv *env, jobject this, jobject f, jint arg)
{
    int length;
    jbyteArray jBlob;
    const void *blob;
    sqlite3_value *value = tovalue(env, f, arg);
    if (!value) return NULL;

    blob = sqlite3_value_blob(value);
    if (!blob) return NULL;

    length = sqlite3_value_bytes(value);
    jBlob = (*env)->NewByteArray(env, length);
    if (!jBlob) { throwex_outofmemory(env); return 0; }

    (*env)->SetByteArrayRegion(env, jBlob, (jsize) 0, (jsize) length, (const jbyte*) blob);

    return jBlob;
}

JNIEXPORT jdouble JNICALL Java_org_sqlite_core_NativeDB_value_1double(
        JNIEnv *env, jobject this, jobject f, jint arg)
{
    sqlite3_value *value = tovalue(env, f, arg);
    return value ? sqlite3_value_double(value) : 0;
}

JNIEXPORT jlong JNICALL Java_org_sqlite_core_NativeDB_value_1long(
        JNIEnv *env, jobject this, jobject f, jint arg)
{
    sqlite3_value *value = tovalue(env, f, arg);
    return value ? sqlite3_value_int64(value) : 0;
}

JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_value_1int(
        JNIEnv *env, jobject this, jobject f, jint arg)
{
    sqlite3_value *value = tovalue(env, f, arg);
    return value ? sqlite3_value_int(value) : 0;
}

JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_value_1type(
        JNIEnv *env, jobject this, jobject func, jint arg)
{
    return sqlite3_value_type(tovalue(env, func, arg));
}


JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_create_1function(
        JNIEnv *env, jobject this, jstring name, jobject func)
{
    jint ret = 0;
    char *name_bytes;
    int isAgg = 0;

    static jfieldID udfdatalist = 0;
    struct UDFData *udf = malloc(sizeof(struct UDFData));

    if (!udf) { throwex_outofmemory(env); return 0; }

    if (!udfdatalist)
        udfdatalist = (*env)->GetFieldID(env, dbclass, "udfdatalist", "J");

    isAgg = (*env)->IsInstanceOf(env, func, aclass);
    udf->func = (*env)->NewGlobalRef(env, func);
    (*env)->GetJavaVM(env, &udf->vm);

    // add new function def to linked list
    udf->next = toref((*env)->GetLongField(env, this, udfdatalist));
    (*env)->SetLongField(env, this, udfdatalist, fromref(udf));

    stringToUtf8Bytes(env, name, &name_bytes, NULL);
    if (!name_bytes) { throwex_outofmemory(env); return 0; }

    ret = sqlite3_create_function(
            gethandle(env, this),
            name_bytes,    // function name
            -1,            // number of args
            SQLITE_UTF16,  // preferred chars
            udf,
            isAgg ? 0 :&xFunc,
            isAgg ? &xStep : 0,
            isAgg ? &xFinal : 0
    );
    freeUtf8Bytes(name_bytes);

    return ret;
}

JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_destroy_1function(
        JNIEnv *env, jobject this, jstring name)
{
    jint ret = 0;
    char* name_bytes;

    stringToUtf8Bytes(env, name, &name_bytes, NULL);
    if (!name_bytes) { throwex_outofmemory(env); return 0; }
    
    ret = sqlite3_create_function(
        gethandle(env, this), name_bytes, -1, SQLITE_UTF16, 0, 0, 0, 0
    );
    freeUtf8Bytes(name_bytes);

    return ret;
}

JNIEXPORT void JNICALL Java_org_sqlite_core_NativeDB_free_1functions(
        JNIEnv *env, jobject this)
{
    // clean up all the malloc()ed UDFData instances using the
    // linked list stored in DB.udfdatalist
    jfieldID udfdatalist;
    struct UDFData *udf, *udfpass;

    udfdatalist = (*env)->GetFieldID(env, dbclass, "udfdatalist", "J");
    udf = toref((*env)->GetLongField(env, this, udfdatalist));
    (*env)->SetLongField(env, this, udfdatalist, 0);

    while (udf) {
        udfpass = udf->next;
        (*env)->DeleteGlobalRef(env, udf->func);
        free(udf);
        udf = udfpass;
    }
}


// COMPOUND FUNCTIONS ///////////////////////////////////////////////

JNIEXPORT jobjectArray JNICALL Java_org_sqlite_core_NativeDB_column_1metadata(
        JNIEnv *env, jobject this, jlong stmt)
{
    const char *zTableName, *zColumnName;
    int pNotNull, pPrimaryKey, pAutoinc, i, colCount;
    jobjectArray array;
    jbooleanArray colData;
    jboolean* colDataRaw;
    sqlite3 *db;
    sqlite3_stmt *dbstmt;

    db = gethandle(env, this);
    dbstmt = toref(stmt);

    colCount = sqlite3_column_count(dbstmt);
    array = (*env)->NewObjectArray(
        env, colCount, (*env)->FindClass(env, "[Z"), NULL) ;
    if (!array) { throwex_outofmemory(env); return 0; }

    colDataRaw = (jboolean*)malloc(3 * sizeof(jboolean));
    if (!colDataRaw) { throwex_outofmemory(env); return 0; }

    for (i = 0; i < colCount; i++) {
        // load passed column name and table name
        zColumnName = sqlite3_column_name(dbstmt, i);
        zTableName  = sqlite3_column_table_name(dbstmt, i);

        pNotNull = 0;
        pPrimaryKey = 0;
        pAutoinc = 0;

        // request metadata for column and load into output variables
        if (zTableName && zColumnName) {
            sqlite3_table_column_metadata(
                db, 0, zTableName, zColumnName,
                0, 0, &pNotNull, &pPrimaryKey, &pAutoinc
            );
        }

        // load relevant metadata into 2nd dimension of return results
        colDataRaw[0] = pNotNull;
        colDataRaw[1] = pPrimaryKey;
        colDataRaw[2] = pAutoinc;

        colData = (*env)->NewBooleanArray(env, 3);
        if (!colData) { throwex_outofmemory(env); return 0; }

        (*env)->SetBooleanArrayRegion(env, colData, 0, 3, colDataRaw);
        (*env)->SetObjectArrayElement(env, array, i, colData);
    }

    free(colDataRaw);

    return array;
}

// backup function

void reportProgress(JNIEnv* env, jobject func, int remaining, int pageCount) {

  static jmethodID mth = 0;
  if (!mth) {
      mth = (*env)->GetMethodID(env, pclass, "progress", "(II)V");
  }

  if(!func) 
    return;

  (*env)->CallVoidMethod(env, func, mth, remaining, pageCount);
}


/*
** Perform an online backup of database pDb to the database file named
** by zFilename. This function copies 5 database pages from pDb to
** zFilename, then unlocks pDb and sleeps for 250 ms, then repeats the
** process until the entire database is backed up.
** 
** The third argument passed to this function must be a pointer to a progress
** function. After each set of 5 pages is backed up, the progress function
** is invoked with two integer parameters: the number of pages left to
** copy, and the total number of pages in the source file. This information
** may be used, for example, to update a GUI progress bar.
**
** While this function is running, another thread may use the database pDb, or
** another process may access the underlying database file via a separate 
** connection.
**
** If the backup process is successfully completed, SQLITE_OK is returned.
** Otherwise, if an error occurs, an SQLite error code is returned.
*/

JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_backup(
  JNIEnv *env, jobject this, 
  jstring zDBName,
  jstring zFilename,          /* Name of file to back up to */
  jobject observer            /* Progress function to invoke */     
)
{
#if SQLITE_VERSION_NUMBER >= 3006011
  int rc;                     /* Function return code */
  sqlite3* pDb;               /* Database to back up */
  sqlite3* pFile;             /* Database connection opened on zFilename */
  sqlite3_backup *pBackup;    /* Backup handle used to copy data */
  char *dFileName;
  char *dDBName;

  pDb = gethandle(env, this);

  stringToUtf8Bytes(env, zFilename, &dFileName, NULL);
  if (!dFileName)
  {
    return SQLITE_NOMEM;
  }

  stringToUtf8Bytes(env, zDBName, &dDBName, NULL);
  if (!dDBName)
  {
    freeUtf8Bytes(dFileName);
    return SQLITE_NOMEM;
  }

  /* Open the database file identified by dFileName. */
  rc = sqlite3_open(dFileName, &pFile);
  if( rc==SQLITE_OK ){

    /* Open the sqlite3_backup object used to accomplish the transfer */
    pBackup = sqlite3_backup_init(pFile, "main", pDb, dDBName);
    if( pBackup ){
      while((rc = sqlite3_backup_step(pBackup,100))==SQLITE_OK ){}

      /* Release resources allocated by backup_init(). */
      (void)sqlite3_backup_finish(pBackup);
    }
    rc = sqlite3_errcode(pFile);
  }

  /* Close the database connection opened on database file zFilename
  ** and return the result of this function. */
  (void)sqlite3_close(pFile);

  freeUtf8Bytes(dDBName);
  freeUtf8Bytes(dFileName);

  return rc;
#else
  return SQLITE_INTERNAL;
#endif
} 

JNIEXPORT jint JNICALL Java_org_sqlite_core_NativeDB_restore(
  JNIEnv *env, jobject this, 
  jstring zDBName,
  jstring zFilename,            /* Name of file to back up to */
  jobject observer              /* Progress function to invoke */    
)
{
#if SQLITE_VERSION_NUMBER >= 3006011
  int rc;                     /* Function return code */
  sqlite3* pDb;               /* Database to back up */
  sqlite3* pFile;             /* Database connection opened on zFilename */
  sqlite3_backup *pBackup;    /* Backup handle used to copy data */
  char *dFileName;
  char *dDBName;
  int nTimeout = 0;

  pDb = gethandle(env, this);

  stringToUtf8Bytes(env, zFilename, &dFileName, NULL);
  if (!dFileName)
  {
    return SQLITE_NOMEM;
  }

  stringToUtf8Bytes(env, zDBName, &dDBName, NULL);
  if (!dDBName)
  {
    freeUtf8Bytes(dFileName);
    return SQLITE_NOMEM;
  }

  /* Open the database file identified by dFileName. */
  rc = sqlite3_open(dFileName, &pFile);
  if( rc==SQLITE_OK ){

    /* Open the sqlite3_backup object used to accomplish the transfer */
    pBackup = sqlite3_backup_init(pDb, dDBName, pFile, "main");
    if( pBackup ){
        while( (rc = sqlite3_backup_step(pBackup,100))==SQLITE_OK
              || rc==SQLITE_BUSY  ){
              if( rc==SQLITE_BUSY ){
                if( nTimeout++ >= 3 ) break;
                sqlite3_sleep(100);
            }
        }
      /* Release resources allocated by backup_init(). */
      (void)sqlite3_backup_finish(pBackup);
    }
    rc = sqlite3_errcode(pFile);
  }

  /* Close the database connection opened on database file zFilename
  ** and return the result of this function. */
  (void)sqlite3_close(pFile);

  freeUtf8Bytes(dDBName);
  freeUtf8Bytes(dFileName);

  return rc;
#else
  return SQLITE_INTERNAL;
#endif
} 
