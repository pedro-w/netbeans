/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * author Tomas Hurka
 *        Ian Formanek
 *        Misha Dmitriev
 */

#ifdef WIN32
#include <Windows.h>
#else
#include <sys/time.h>
#include <fcntl.h>
#include <time.h>
#endif

#ifdef SOLARIS
#define _STRUCTURED_PROC 1
#include <sys/procfs.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>

#include "jni.h"
#include "jvmti.h"

#include "org_netbeans_lib_profiler_server_system_Stacks.h"

#include "common_functions.h"
#include "Threads.h"

#define NEEDS_CONVERSION (sizeof(jmethodID)!=sizeof(jint))
#define NO_OF_BASE_BITS 2
#define NO_OF_MASK_BITS (32-NO_OF_BASE_BITS)
#define NO_OF_BASE_ADDRESS (1<<NO_OF_BASE_BITS)
#define OFFSET_MASK ((1L<<NO_OF_MASK_BITS)-1)
#define BASE_ADDRESS_MASK (~OFFSET_MASK)

#define MAX_FRAMES 16384

#define PACKEDARR_ITEMS 4

static jvmtiFrameInfo *_stack_frames_buffer = NULL;
static jint *_stack_id_buffer = NULL;
static jclass threadType = NULL;
static jclass intArrType = NULL;
static long base_addresses[NO_OF_BASE_ADDRESS]={-1L,-1L,-1L,-1L};
struct entry { 
    jmethodID id;
    jint key; 
};
static jmethodID* ids = NULL;
static int capacity;
struct entry* entries = NULL;
static int size;
static int nElements;
static int threshold;


static uint32_t jmethodid_hashcode(jmethodID jmethod) {
    // This is an unsigned version of Long#hashCode
    uintptr_t ptr = (intptr_t) jmethod;
    return (uint32_t)(ptr ^ (ptr >> 32));
}
static void growTable() {
    fprintf(stderr, "*** Now growing table\n");
    struct entry* old = entries;
    int oldsize = size, i;
    size = (size * 2) + 1;
    threshold = size * 3 / 4;
    entries = calloc(size, sizeof(struct entry));
    for (i=0; i < oldsize; ++i) {
        if (old[i].id) {
            int pos = jmethodid_hashcode(old[i].id) % size;
            while (entries[pos].id) {
                pos = (pos + 1) % size;
            }
            entries[pos].id = old[i].id;
            entries[pos].key = old[i].key;
        }
    }
    free(old);
}
static void initTable() {
    fprintf(stderr, "*** now setting up table\n");
    capacity = size = 97;
    nElements = 0;
    threshold = size * 3 / 4;
    ids = calloc(capacity, sizeof(jmethodID));
    entries = calloc(size, sizeof(struct entry));
}
static jint convert_jmethodID_to_jint(jmethodID jmethod) {
    if (NEEDS_CONVERSION) {
        assert(entries);
        int pos = jmethodid_hashcode(jmethod) % size;
        assert(pos >=0 && pos < size);
        while (entries[pos].id) {
            if (entries[pos].id == jmethod) {
                return entries[pos].key;
            }
            pos = (pos + 1) % size;
        }
        if (nElements < threshold) {
            entries[pos].id = jmethod;
            entries[pos].key = nElements;
            fprintf(stderr, "*** Now convert %p to %d\n", jmethod, nElements);
            if (nElements >= capacity) {
                jmethodID* oldids = ids;
                capacity *= 2;
                ids = calloc(capacity, sizeof(jmethodID));
                memcpy(ids, oldids, nElements * sizeof(jmethodID));
                free(oldids);
            }
            ids[nElements] = jmethod;
            nElements++;
            return entries[pos].key;
        } else {
            growTable();
            return convert_jmethodID_to_jint(jmethod);
        }
    } else {
        return (jint)(intptr_t)jmethod;
    }
}

static jmethodID convert_jint_to_jmethodID(jint method) {
    if (NEEDS_CONVERSION) {
        fprintf(stderr, "*** Now unconvert %d to %p\n", method, ids[method]);
        return ids[method];
    } else {
        return (jmethodID)(intptr_t)method;
    }
}

/*
 * Class:     org_netbeans_lib_profiler_server_system_Stacks
 * Method:    getCurrentJavaStackDepth
 * Signature: (Ljava/lang/Thread;)I
 */
JNIEXPORT jint JNICALL Java_org_netbeans_lib_profiler_server_system_Stacks_getCurrentJavaStackDepth
    (JNIEnv *env, jclass clz, jobject jni_thread)
{
    jint count;

    (*_jvmti)->GetFrameCount(_jvmti, jni_thread, &count);
    return count;
}


/*
 * Class:     org_netbeans_lib_profiler_server_system_Stacks
 * Method:    createNativeStackFrameBuffer
 * Signature: (I)V
 */
JNIEXPORT void JNICALL Java_org_netbeans_lib_profiler_server_system_Stacks_createNativeStackFrameBuffer
    (JNIEnv *env, jclass clz, jint sizeInFrames)
{
    if (_stack_frames_buffer != NULL) {
        Java_org_netbeans_lib_profiler_server_system_Stacks_clearNativeStackFrameBuffer(env, clz);
    }
    _stack_frames_buffer = calloc(sizeInFrames, sizeof(jvmtiFrameInfo));
    _stack_id_buffer = calloc(sizeInFrames, sizeof(jint));
}


/*
 * Class:     org_netbeans_lib_profiler_server_system_Stacks
 * Method:    clearNativeStackFrameBuffer
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_netbeans_lib_profiler_server_system_Stacks_clearNativeStackFrameBuffer
    (JNIEnv *env, jclass clz)
{
    if (_stack_frames_buffer != NULL) {
        free(_stack_frames_buffer);
    }
    if (_stack_id_buffer != NULL) {
        free(_stack_id_buffer);
    }
    _stack_frames_buffer = NULL;
    _stack_id_buffer = NULL;
}


/*
 * Class:     org_netbeans_lib_profiler_server_system_Stacks
 * Method:    getCurrentStackFrameIds
 * Signature: (Ljava/lang/Thread;I[I)I
 */
JNIEXPORT jint JNICALL Java_org_netbeans_lib_profiler_server_system_Stacks_getCurrentStackFrameIds
    (JNIEnv *env, jclass clz, jthread jni_thread, jint depth, jintArray ret)
{
    jint i, count;
    if (_stack_frames_buffer == NULL) {
        /* Can happen if profiling stopped concurrently */
        return 0;
    }

    (*_jvmti)->GetStackTrace(_jvmti, jni_thread, 0, depth, _stack_frames_buffer, &count);
    if (entries == NULL) {
        initTable();
    }
    for (i = 0; i < count; i++) {
        _stack_id_buffer[i] = convert_jmethodID_to_jint(_stack_frames_buffer[i].method);
    }
    (*env)->SetIntArrayRegion(env, ret, 0, count, _stack_id_buffer);

    return count;
}


static jbyte *byteData;
static jint *strOffsets;
static int byteDataLen, dataOfs, ofsIdx;

static void copy_into_data_array(char *s) {
    int len = strlen(s);
    if (dataOfs + len > byteDataLen) {
        jbyte *oldByteData = byteData;
        int newLen = byteDataLen * 2;

        if (newLen < dataOfs + len) {
          newLen = dataOfs+len;
        }
        byteData = malloc(newLen);
        memcpy(byteData, oldByteData, dataOfs);
        free(oldByteData);
        byteDataLen = newLen;
    }

    strncpy((char*)(byteData + dataOfs), s, len);
    strOffsets[ofsIdx++] = dataOfs;
    dataOfs += len;
}

static void copy_dummy_names_into_data_array() {
    copy_into_data_array("<unknown class>");
    copy_into_data_array("<unknown method>");
    copy_into_data_array("()V");
    copy_into_data_array("0");
}


/*
 * Class:     org_netbeans_lib_profiler_server_system_Stacks
 * Method:    getMethodNamesForJMethodIds
 * Signature: (I[I[I)[B
 */
JNIEXPORT jbyteArray JNICALL Java_org_netbeans_lib_profiler_server_system_Stacks_getMethodNamesForJMethodIds
  (JNIEnv *env, jclass clz, jint nMethods, jintArray jmethodIds, jintArray packedArrayOffsets)
{
    jvmtiError res;
    int i, len;
    jint *methodIds;
    jbyteArray ret;

    // fprintf (stderr, "1");
    methodIds = (jint*) malloc(sizeof(jint) * nMethods);
    (*env)->GetIntArrayRegion(env, jmethodIds, 0, nMethods, methodIds);
    strOffsets = (jint*) malloc(sizeof(jint) * nMethods * PACKEDARR_ITEMS);
    byteDataLen = nMethods * PACKEDARR_ITEMS * 10;  /* The initial size for the packed strings array */
    byteData = (jbyte*) malloc(byteDataLen);

    // fprintf (stderr, "2");
    dataOfs = ofsIdx = 0;

    for (i = 0; i < nMethods; i++) {
        jclass declaringClass;
        char *className, *genericSignature, *methodName, *methodSig, *genericMethodSig;
        jboolean native = JNI_FALSE;
        jmethodID methodID = convert_jint_to_jmethodID(methodIds[i]);

        //fprintf (stderr, "Going to call GetMethodDeclaringClass for methodId = %d\n", *(int*)methodID);

        res = (*_jvmti)->GetMethodDeclaringClass(_jvmti, methodID, &declaringClass);
        if (res != JVMTI_ERROR_NONE || declaringClass == NULL || *((int*)declaringClass) == 0) { /* Also a bug workaround */
            fprintf(stderr, "Profiler Agent Warning: Invalid declaringClass obtained from jmethodID\n");
            fprintf(stderr, "Profiler Agent Warning: mId = %p, *mId = %d\n", methodID, *(int*)methodID);
            fprintf(stderr, "Profiler Agent Warning: dCl = %p", declaringClass);
            if (declaringClass != NULL) {
                fprintf(stderr, ", *dCl = %d\n", *((int*)declaringClass));
            } else {
                fprintf(stderr, "\n");
            }
            // fprintf(stderr, "*** res = %d", res);
            copy_dummy_names_into_data_array();
            continue;
        }

        // fprintf (stderr, "Going to call GetClassSignature for methodId = %d, last res = %d, declaring class: %d\n", *(int*)methodID, res, *((int*)declaringClass));

        res = (*_jvmti)->GetClassSignature(_jvmti, declaringClass, &className, &genericSignature);
        if (res != JVMTI_ERROR_NONE) {
            fprintf(stderr, "Profiler Agent Warning: Couldn't obtain name of declaringClass = %p\n", declaringClass);
            copy_dummy_names_into_data_array();
            continue;
        }

        // fprintf (stderr, "Going to call GetMethodName for methodId = %d, last res = %d, signature: %s\n", *(int*)methodID, res, genericSignature);

        res = (*_jvmti)->GetMethodName(_jvmti, methodID, &methodName, &methodSig, &genericMethodSig);

        if (res != JVMTI_ERROR_NONE) {
            fprintf(stderr, "Profiler Agent Warning: Couldn't obtain name for methodID = %p\n", methodID);
            copy_dummy_names_into_data_array();
            continue;
        }

        // fprintf (stderr, "Going to call IsMethodNative for methodId = %d, last res = %d, signature: %s\n", *(int*)methodID, res, genericSignature);
        
        res = (*_jvmti)->IsMethodNative(_jvmti, methodID, &native);
        
        if (res != JVMTI_ERROR_NONE) {
            fprintf(stderr, "Profiler Agent Warning: Couldn't obtain native flag for methodID = %p\n", methodID);
        }

        // fprintf (stderr, "Going to copy results, last res = %d, method name: %s, sig: %s, genSig: %s, native %d\n", res, methodName, methodSig, genericMethodSig, native);

        len = strlen(className);
        if (className[0] == 'L' && className[len-1] == ';') {
            className[len-1] = 0;
            copy_into_data_array(className+1);
        } else {
            copy_into_data_array(className);
        }

        copy_into_data_array(methodName);
        copy_into_data_array(methodSig);
        copy_into_data_array(native?"1":"0");

        (*_jvmti)->Deallocate(_jvmti, (void*)className);

        if (genericSignature != NULL) {
            (*_jvmti)->Deallocate(_jvmti, (void*)genericSignature);
        }

        (*_jvmti)->Deallocate(_jvmti, (void*)methodName);
        (*_jvmti)->Deallocate(_jvmti, (void*)methodSig);
        if (genericMethodSig != NULL) {
            (*_jvmti)->Deallocate(_jvmti, (void*)genericMethodSig);
        }
    }

    // fprintf (stderr, "3");
    free(methodIds);

    ret = (*env)->NewByteArray(env, dataOfs);
    (*env)->SetByteArrayRegion(env, ret, 0, dataOfs, byteData);
    (*env)->SetIntArrayRegion(env, packedArrayOffsets, 0, nMethods*PACKEDARR_ITEMS, strOffsets);

    // fprintf (stderr, "4");
    free(strOffsets);
    free(byteData);

    return ret;
}


/*
 * Class:     org_netbeans_lib_profiler_server_system_Stacks
 * Method:    getAllStackTraces
 * Signature: ([[Ljava/lang/Thread;[[I[[[I)V
 */
JNIEXPORT void JNICALL Java_org_netbeans_lib_profiler_server_system_Stacks_getAllStackTraces
  (JNIEnv *env, jclass clz, jobjectArray threads, jobjectArray states, jobjectArray frames)
{
    jobjectArray jthreadArr;
    jobjectArray statesArr;
    jobjectArray methodIdArrArr;
    jvmtiStackInfo *stack_info;
    jint *state_buffer;
    jint thread_count;
    int ti;
    jvmtiError err;

    err = (*_jvmti)->GetAllStackTraces(_jvmti, MAX_FRAMES, &stack_info, &thread_count); 
    if (err != JVMTI_ERROR_NONE) {
       return;
    }
    if (threadType == NULL) {
        threadType = (*env)->FindClass(env, "java/lang/Thread");
        threadType = (*env)->NewGlobalRef(env, threadType);
    }
    if (intArrType == NULL) {
        intArrType = (*env)->FindClass(env, "[I");
        intArrType = (*env)->NewGlobalRef(env, intArrType);
    }
    if (entries == NULL) {
        initTable();
    }
    jthreadArr = (*env)->NewObjectArray(env, thread_count, threadType, NULL);
    (*env)->SetObjectArrayElement(env, threads, 0, jthreadArr);
    statesArr = (*env)->NewIntArray(env, thread_count);
    (*env)->SetObjectArrayElement(env, states, 0, statesArr);
    methodIdArrArr = (*env)->NewObjectArray(env, thread_count, intArrType, NULL);
    (*env)->SetObjectArrayElement(env, frames, 0, methodIdArrArr);    
    state_buffer = calloc(thread_count, sizeof(jint));
    
    for (ti = 0; ti < thread_count; ti++) {
       jvmtiStackInfo *infop = &stack_info[ti];
       jthread thread = infop->thread;
       jint state = infop->state;
       jvmtiFrameInfo *frames = infop->frame_buffer;
       jobjectArray jmethodIdArr;
       jint *id_buffer;
       int fi;

       (*env)->SetObjectArrayElement(env, jthreadArr, ti, thread);
       state_buffer[ti] = convert_JVMTI_thread_status_to_jfluid_status(state);
       
       jmethodIdArr = (*env)->NewIntArray(env, infop->frame_count);
       (*env)->SetObjectArrayElement(env, methodIdArrArr, ti, jmethodIdArr);    
       id_buffer = calloc(infop->frame_count, sizeof(jint));
       for (fi = 0; fi < infop->frame_count; fi++) {
          id_buffer[fi] = convert_jmethodID_to_jint(frames[fi].method);
       }
       (*env)->SetIntArrayRegion(env, jmethodIdArr, 0, infop->frame_count, id_buffer);
       free(id_buffer);
    }
    (*env)->SetIntArrayRegion(env, statesArr, 0, thread_count, state_buffer);
    
    /* this one Deallocate call frees all data allocated by GetAllStackTraces */
    err = (*_jvmti)->Deallocate(_jvmti, (unsigned char*)stack_info);
    assert(err == JVMTI_ERROR_NONE);
    free(state_buffer);
}
