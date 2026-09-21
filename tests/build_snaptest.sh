#!/bin/bash
SDK=/Users/bwandrych/Developer/PlaydateSDK
SRCS="tests/snap_host_test.c /tmp/snap_shim.c
Source/core/pluto_snap.c Source/core/pluto_spill.c Source/core/pluto_mem.c Source/core/logger.c
Source/html/document.c Source/html/dom.c Source/html/entities.c Source/html/tokenizer.c Source/html/readability.c Source/html/css.c
Source/core/url.c Source/core/constants.c Source/core/tasks.c Source/core/http_client.c Source/core/encoding.c Source/core/cookie_jar.c Source/core/storage.c
Source/util/strbuf.c Source/util/strutil.c Source/util/json.c Source/util/pdtimer.c
Source/html/jsbridge.c Source/html/jsext.c
Source/render/decoders/scale.c Source/render/decoders/dither.c Source/render/decoders/inflate.c Source/render/decoders/png.c
Source/js/muJS/*.c Source/js/duktape/duktape.c
Source/html/qjs_shim_quickjs.c Source/html/qjs_shim_libregexp.c Source/html/qjs_shim_libunicode.c Source/html/qjs_shim_cutils.c Source/html/qjs_shim_dtoa.c Source/html/qjs_pthread_stubs.c
Source/html/jsbridge_mujs.c Source/html/jsbridge_duktape.c Source/html/jsbridge_quickjs.c Source/html/jsbridge_xs.c
Source/js/xs_moddable/sources/xsAll.c Source/js/xs_moddable/sources/xsAPI.c Source/js/xs_moddable/sources/xsArguments.c Source/js/xs_moddable/sources/xsArray.c
Source/js/xs_moddable/sources/xsAtomics.c Source/js/xs_moddable/sources/xsBigInt.c Source/js/xs_moddable/sources/xsBoolean.c Source/js/xs_moddable/sources/xsCode.c
Source/js/xs_moddable/sources/xsCommon.c Source/js/xs_moddable/sources/xsDataView.c Source/js/xs_moddable/sources/xsDate.c Source/js/xs_moddable/sources/xsDebug.c
Source/js/xs_moddable/sources/xsDefaults.c Source/js/xs_moddable/sources/xsError.c Source/js/xs_moddable/sources/xsFunction.c Source/js/xs_moddable/sources/xsGenerator.c
Source/js/xs_moddable/sources/xsGlobal.c Source/js/xs_moddable/sources/xsJSON.c Source/js/xs_moddable/sources/xsLexical.c Source/js/xs_moddable/sources/xsLockdown.c
Source/js/xs_moddable/sources/xsMapSet.c Source/js/xs_moddable/sources/xsMarshall.c Source/js/xs_moddable/sources/xsMath.c Source/js/xs_moddable/sources/xsMemory.c
Source/js/xs_moddable/sources/xsModule.c Source/js/xs_moddable/sources/xsNumber.c Source/js/xs_moddable/sources/xsObject.c Source/js/xs_moddable/sources/xsPlatforms.c
Source/js/xs_moddable/sources/xsProfile.c Source/js/xs_moddable/sources/xsPromise.c Source/js/xs_moddable/sources/xsProperty.c Source/js/xs_moddable/sources/xsProxy.c
Source/js/xs_moddable/sources/xsRegExp.c Source/js/xs_moddable/sources/xsRun.c Source/js/xs_moddable/sources/xsScope.c Source/js/xs_moddable/sources/xsScript.c
Source/js/xs_moddable/sources/xsSourceMap.c Source/js/xs_moddable/sources/xsString.c Source/js/xs_moddable/sources/xsSymbol.c Source/js/xs_moddable/sources/xsSyntaxical.c
Source/js/xs_moddable/sources/xsTree.c Source/js/xs_moddable/sources/xsType.c Source/js/xs_moddable/sources/xsdtoa.c Source/js/xs_moddable/sources/xsre.c Source/js/xs_moddable/sources/xsmc.c"
cc -o /tmp/snaptest $SRCS \
 -I. -ISource -ISource/core -ISource/util -ISource/html -ISource/render -ISource/render/decoders -I$SDK/C_API \
 -I Source/js/muJS -I Source/js/duktape -I Source/js/QuickJS -I Source/js/xs_moddable/sources -I Source/js/xs_moddable/platforms \
 -DPLUTO_SPILL_HOST -DTARGET_SIMULATOR=1 -DTARGET_EXTENSION=1 -DCONFIG_VERSION=\"2026-06-04\" \
 -Dpthread_mutex_lock=pluto_qjs_pthread_mutex_lock -Dpthread_mutex_unlock=pluto_qjs_pthread_mutex_unlock \
 -Dpthread_cond_init=pluto_qjs_pthread_cond_init -Dpthread_cond_destroy=pluto_qjs_pthread_cond_destroy \
 -Dpthread_cond_signal=pluto_qjs_pthread_cond_signal -Dpthread_cond_wait=pluto_qjs_pthread_cond_wait \
 -Dpthread_cond_timedwait=pluto_qjs_pthread_cond_timedwait -Dclock_gettime=pluto_qjs_clock_gettime \
 -DINCLUDE_XSPLATFORM -DXSPLATFORM=\"xs_platform.h\" \
 -DJS_ASTLIMIT=48 -DJS_ENVLIMIT=64 -DJS_TRYLIMIT=8 -DREG_MAXREC=48 -DREG_MAXCLASS=16 -DJS_STACKSIZE=2048 \
 -fsanitize=address,undefined
