/*
 * Copyright (c) 2016-2026  Moddable Tech, Inc.
 *
 *   This file is part of the Moddable SDK Runtime.
 * 
 *   The Moddable SDK Runtime is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 * 
 *   The Moddable SDK Runtime is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Lesser General Public License for more details.
 * 
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with the Moddable SDK Runtime.  If not, see <http://www.gnu.org/licenses/>.
 *
 * This file incorporates work covered by the following copyright and  
 * permission notice:  
 *
 *       Copyright (C) 2010-2016 Marvell International Ltd.
 *       Copyright (C) 2002-2010 Kinoma, Inc.
 *
 *       Licensed under the Apache License, Version 2.0 (the "License");
 *       you may not use this file except in compliance with the License.
 *       You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *       Unless required by applicable law or agreed to in writing, software
 *       distributed under the License is distributed on an "AS IS" BASIS,
 *       WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *       See the License for the specific language governing permissions and
 *       limitations under the License.
 */

#include "xsScript.h"
extern void fxAbort(txMachine* the, int status);

#if defined(__clang__) || defined (__GNUC__)
	__attribute__((no_sanitize_address))
#endif
void fxCheckParserStack(txParser* parser, txInteger line)
{
    char x;
    char *stack = &x;
    if (stack <= parser->stackLimit) {
    	fxAbort(parser->console, XS_NATIVE_STACK_OVERFLOW_EXIT);
    }
}

txString fxCombinePath(txParser* parser, txString base, txString name)
{
	txSize baseLength, nameLength;
	txString path;
	txString separator ;
#if mxWindows
	separator = name;
	while (*separator) {
		if (*separator == '/')
			*separator = '\\';
		separator++;
	}
	separator = strrchr(base, '\\');
#else
	separator = strrchr(base, '/');
#endif
	if (separator) {
		separator++;
		baseLength = mxPtrDiff(separator - base);
	}
	else
		baseLength = 0;
	nameLength = mxStringLength(name);
	path = fxNewParserChunk(parser, baseLength + nameLength + 1);
	if (baseLength)
		c_memcpy(path, base, baseLength);
	c_memcpy(path + baseLength, name, nameLength + 1);
	return path;
}

void fxDisposeParserChunks(txParser* parser)
{
	txParserChunk** address = &parser->first;
	txParserChunk* block;
	while ((block = *address)) {
		*address = block->next;
		c_free(block);
	}
}

void fxInitializeParser(txParser* parser, void* console, txSize bufferSize, txSize symbolModulo)
{
	c_memset(parser, 0, sizeof(txParser));
	parser->first = C_NULL;
	parser->console = console;
	parser->stackLimit = fxCStackLimit();
	parser->curFrame = C_NULL;
	parser->framePool = C_NULL;
	parser->pumpRunning = 0;
	
	parser->symbolModulo = symbolModulo;
	parser->symbolTable = fxNewParserChunkClear(parser, parser->symbolModulo * sizeof(txSymbol*));

	parser->emptyString = fxNewParserString(parser, "", 0);

	parser->ObjectSymbol = fxNewParserSymbol(parser, "Object");
	parser->__dirnameSymbol = fxNewParserSymbol(parser, "__dirname");
	parser->__filenameSymbol = fxNewParserSymbol(parser, "__filename");
	parser->__jsx__Symbol = fxNewParserSymbol(parser, "__jsx__");
	parser->__proto__Symbol = fxNewParserSymbol(parser, "__proto__");
	parser->allSymbol = fxNewParserSymbol(parser, "*");
	parser->argsSymbol = fxNewParserSymbol(parser, "args");
	parser->argumentsSymbol = fxNewParserSymbol(parser, "arguments");
	parser->arrowSymbol = fxNewParserSymbol(parser, "=>");
	parser->asSymbol = fxNewParserSymbol(parser, "as");
	parser->asyncSymbol = fxNewParserSymbol(parser, "async");
	parser->awaitSymbol = fxNewParserSymbol(parser, "await");
	parser->callSymbol = fxNewParserSymbol(parser, "call");
	parser->callerSymbol = fxNewParserSymbol(parser, "caller");
	parser->constructorSymbol = fxNewParserSymbol(parser, "constructor");
	parser->defaultSymbol = fxNewParserSymbol(parser, "default");
	parser->doneSymbol = fxNewParserSymbol(parser, "done");
	parser->evalSymbol = fxNewParserSymbol(parser, "eval");
	parser->exportsSymbol = fxNewParserSymbol(parser, "exports");
	parser->fillSymbol = fxNewParserSymbol(parser, "fill");
	parser->freezeSymbol = fxNewParserSymbol(parser, "freeze");
	parser->fromSymbol = fxNewParserSymbol(parser, "from");
	parser->getSymbol = fxNewParserSymbol(parser, "get");
	parser->idSymbol = fxNewParserSymbol(parser, "id");
	parser->includeSymbol = fxNewParserSymbol(parser, "include");
	parser->InfinitySymbol = fxNewParserSymbol(parser, "Infinity");
	parser->jsonSymbol = fxNewParserSymbol(parser, "json");
	parser->lengthSymbol = fxNewParserSymbol(parser, "length");
	parser->letSymbol = fxNewParserSymbol(parser, "let");
	parser->metaSymbol = fxNewParserSymbol(parser, "meta");
	parser->moduleSymbol = fxNewParserSymbol(parser, "module");
	parser->nameSymbol = fxNewParserSymbol(parser, "name");
	parser->NaNSymbol = fxNewParserSymbol(parser, "NaN");
	parser->NativeSymbol = fxNewParserSymbol(parser, "Native");
	parser->nativeSymbol = fxNewParserSymbol(parser, "native");
	parser->nextSymbol = fxNewParserSymbol(parser, "next");
	parser->newTargetSymbol = fxNewParserSymbol(parser, "new.target");
	parser->ofSymbol = fxNewParserSymbol(parser, "of");
	parser->privateConstructorSymbol = fxNewParserSymbol(parser, "#constructor");
	parser->prototypeSymbol = fxNewParserSymbol(parser, "prototype");
	parser->RangeErrorSymbol = fxNewParserSymbol(parser, "RangeError");
	parser->rawSymbol = fxNewParserSymbol(parser, "raw");
	parser->returnSymbol = fxNewParserSymbol(parser, "return");
	parser->setSymbol = fxNewParserSymbol(parser, "set");
	parser->sliceSymbol = fxNewParserSymbol(parser, "slice");
	parser->SyntaxErrorSymbol = fxNewParserSymbol(parser, "SyntaxError");
	parser->staticSymbol = fxNewParserSymbol(parser, "static");
	parser->StringSymbol = fxNewParserSymbol(parser, "String");
	parser->targetSymbol = fxNewParserSymbol(parser, "target");
	parser->thisSymbol = fxNewParserSymbol(parser, "this");
	parser->throwSymbol = fxNewParserSymbol(parser, "throw");
	parser->toStringSymbol = fxNewParserSymbol(parser, "toString");
	parser->undefinedSymbol = fxNewParserSymbol(parser, "undefined");
	parser->uriSymbol = fxNewParserSymbol(parser, "uri");
	parser->usingSymbol = fxNewParserSymbol(parser, "using");
	parser->valueSymbol = fxNewParserSymbol(parser, "value");
	parser->withSymbol = fxNewParserSymbol(parser, "with");
	parser->yieldSymbol = fxNewParserSymbol(parser, "yield");
	
	parser->errorSymbol = NULL;
	parser->reportError = fxVReportError;
	parser->reportWarning = fxVReportWarning;

	parser->buffer = fxNewParserChunk(parser, bufferSize);
	parser->bufferSize = bufferSize;
}

/* R13: 128KB chunk refills instead of 4KB. A 500KB-input parse needs
   ~1.3MB of parser memory; as 4KB blocks that was ~330 separate SDK mallocs
   interleaved with the page machinery's allocations — the device allocator
   wedged at only ~3MB live (fragmentation, not exhaustion: the pool is
   ~7.5MB). Large blocks give the allocator contiguous regions and cut the
   allocation count ~30x. Transient waste is bounded (freed at parser
   terminate) and small pages still fit in one block. kParserChunkSize now
   lives in xsScript.h (shared with the R19 pump validator). */

/* xs_no_recursion (R13): parser-memory ceiling, set by the host (bytes; 0 =
   unlimited — simulator/harness). The XS parser needs ~14B of parser memory
   per source byte; on Playdate the SDK heap collapses (allocator wedge /
   watchdog reset) before a clean OOM is possible when a giant script is
   compiled alongside a running machine. Enforced at this single funnel so
   the host gets the stock-contained OOM path (fxReportMemoryError → bridge
   catch → engine reset → page continues) instead of starving the platform
   allocator. Device bridge sets 768KB (observed collapse onset: 0.99–1.3MB). */
unsigned long fxNRParserTotalCap = 0;

#if !defined(TARGET_PLAYDATE) && defined(PLUTO_NR_CAP_DIAG)
/* Temporary R26g parse-memory forensics: per-parser allocation histogram. */
txSize diagAllocs[10]; txSize diagBytes[10]; txSize diagTotal; txSize diagCount;
#endif

/* R26f: the cap above is a PARSE-memory ceiling (~14B/source byte). The
   coder (fxParserCode) allocates every bytecode node through this same
   funnel, so an admission grant sized for the parse still starves CODEGEN
   — the cap fired mid-codegen, fxParserCode built its throw-script, and
   the RUN slice surfaced "script too large (parser memory cap)" for
   segments whose parse fit comfortably (118KB react-dom on device,
   2026-10-04). A codegen phase flag now exempts the coder from the
   parse-only cap: it is raised by fxParserCode around the tree walk and
   cleared on exit. The platform allocator's real exhaustion path (c_malloc
   fail → fxAbort → contained engine reset) remains the backstop, which is
   exactly how the 253KB tail compiled before this flag existed. */
int fxNRParserCodegen = 0;

/* R26g: cap-scoped ATTEMPT mode. A headroom-scoped cap only bounds the parse
   phase while the coder runs exempt — uncapped codegen allocations then go
   straight to the platform allocator and a pool exhaustion there is fxAbort
   (engine reset), not a contained parser error. When the bridge attempts a
   segment whose ESTIMATE exceeds grantable headroom, it sets this flag so
   the cap covers codegen too: worst case is the contained "script too large"
   throw, never a pool blowout. */
int fxNRParserCodegenCapped = 0;

void* fxNewParserChunk(txParser* parser, txSize size)
{
#if !defined(TARGET_PLAYDATE) && defined(PLUTO_NR_CAP_DIAG)
	size = (size + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
	{
		int b = size <= 16 ? 0 : size <= 24 ? 1 : size <= 32 ? 2 : size <= 48 ? 3
			: size <= 64 ? 4 : size <= 128 ? 5 : size <= 256 ? 6 : size <= 1024 ? 7
			: size <= 16384 ? 8 : 9;
		diagAllocs[b]++; diagBytes[b] += size; diagTotal += size; diagCount++;
	}
#endif
	size = (size + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
	if (fxNRParserTotalCap && (!fxNRParserCodegen || fxNRParserCodegenCapped) &&
		(parser->total + size > fxNRParserTotalCap)) {
		/* R16: fxReportMemoryError formats its message through
		   fxNewParserString → fxNewParserChunk, which re-entered this cap
		   check and recursed forever (stack overflow = hard crash). The
		   parser is not yet in the error path iff errorCount == 0; while
		   the error message itself is being built (errorCount > 0) the
		   small allocation must go through so the error path can finish
		   and longjmp out containedly. Post-error allocations only happen
		   inside fxParserCode's throw-script (a few opcodes) — bounded. */
		if (parser->errorCount == 0) {
#if !defined(TARGET_PLAYDATE) && defined(PLUTO_NR_CAP_DIAG)
			fprintf(stderr, "[capdiag] cap %luKB fired: parser->total=%luKB (+%zuB) line %d\n",
				(unsigned long)(fxNRParserTotalCap >> 10),
				(unsigned long)(parser->total >> 10), size,
				(int)parser->states[2].line);
#endif
			fxReportMemoryError(parser, parser->states[2].line, "script too large (parser memory cap)");
		}
	}
	if (size <= parser->chunkSize) {
		void *result = parser->chunk;
		parser->chunk += size;
		parser->chunkSize -= size;
		return result;
	}

	if (size > (txSize)(kParserChunkSize - sizeof(txParserChunk))) {
		txParserChunk *block = c_malloc(sizeof(txParserChunk) + size);
		if (!block)
			fxAbort(parser->console, XS_NOT_ENOUGH_MEMORY_EXIT);
		parser->total += sizeof(txParserChunk) + size;
		block->next = parser->first;
		parser->first = block;
		return block + 1;
	}

	txParserChunk *block = c_malloc(kParserChunkSize);
	if (!block)
		fxAbort(parser->console, XS_NOT_ENOUGH_MEMORY_EXIT);
	parser->total += kParserChunkSize;
	block->next = parser->first;
	parser->first = block;
	parser->chunk = (txByte*)(block + 1) + size;
	parser->chunkSize = kParserChunkSize - sizeof(txParserChunk) - size;
	return block + 1;
}

void* fxNewParserChunkClear(txParser* parser, txSize size)
{
	void* result = fxNewParserChunk(parser, size);
    c_memset(result, 0, size);
	return result;
}

txString fxNewParserString(txParser* parser, txString buffer, txSize size)
{
	if (parser->buffer) { 
		txString result = fxNewParserChunk(parser, size + 1);
		c_memcpy(result, buffer, size);
		result[size] = 0;
		return result;
	}
	return buffer;
}

txSymbol* fxNewParserSymbol(txParser* parser, txString theString)
{
	txString aString;
	txSize aLength;
	txU4 aSum;
	txU4 aModulo;
	txSymbol* aSymbol;
	
	aString = theString;
	aLength = 0;
	aSum = 0;
	while(*aString != 0) {
		aLength++;
		aSum = (aSum << 1) + *aString++;
	}
	aSum &= 0x7FFFFFFF;
	aModulo = aSum % parser->symbolModulo;
	aSymbol = parser->symbolTable[aModulo];
	/* R19: a corrupted bucket chain (cycle in ->next) would spin here
	   forever (device watchdog freeze, observed at seg ~62). Bound the walk;
	   on blowout, log the stalled symbol name and abort containedly. */
	{
		unsigned long walkGuard = 0;
		while (aSymbol != C_NULL) {
			if (aSymbol->sum == aSum)
				if (c_strcmp(aSymbol->string, theString) == 0)
					break;
			aSymbol = aSymbol->next;
			if (++walkGuard > 8192) {
				extern long gXSNRFaultKind;
				extern void* gXSNRFaultFrame;
				extern char gXSNRFaultSym[];
				gXSNRFaultKind = -100 - (long)aModulo;
				gXSNRFaultFrame = aSymbol;
				{
					txString s = theString ? theString : (txString)"";
					txSize n = 0;
					while (s[n] && (n < 31)) { gXSNRFaultSym[n] = s[n]; n++; }
					gXSNRFaultSym[n] = 0;
				}
				fxAbort(parser->console, XS_PUMP_CORRUPTION_EXIT);
			}
		}
	}
	if (aSymbol == C_NULL) {
		aSymbol = fxNewParserChunk(parser, sizeof(txSymbol));
		aSymbol->next = parser->symbolTable[aModulo];
		aSymbol->ID = -1;
		aSymbol->length = aLength + 1;
		aSymbol->string = fxNewParserString(parser, theString, aLength);
		aSymbol->sum = aSum;
		aSymbol->usage = 0;
		parser->symbolTable[aModulo] = aSymbol;
	}
	return aSymbol;
}

void fxReportMemoryError(txParser* parser, txInteger line, txString theFormat, ...)
{
	c_va_list arguments;
	parser->error = C_ENOMEM;
	parser->errorCount++;
	c_va_start(arguments, theFormat);
    (*parser->reportError)(parser->console, parser->path ? parser->path->string : C_NULL, line, theFormat, arguments);
	c_va_end(arguments);
	if (parser->console) {
		parser->errorSymbol = parser->RangeErrorSymbol;
		if (parser->buffer != theFormat) {
			c_va_start(arguments, theFormat);
			c_vsnprintf(parser->buffer, parser->bufferSize, theFormat, arguments);
			c_va_end(arguments);
		}
		parser->errorMessage = fxNewParserString(parser, parser->buffer, mxStringLength(parser->buffer));
	}
	c_longjmp(parser->firstJump->jmp_buf, 1);
}

void fxReportParserError(txParser* parser, txInteger line, txString theFormat, ...)
{
	c_va_list arguments;
	parser->error = C_EINVAL;
	parser->errorCount++;
	c_va_start(arguments, theFormat);
    (*parser->reportError)(parser->console, parser->path ? parser->path->string : C_NULL, line, theFormat, arguments);
	c_va_end(arguments);
	if (parser->console) {
		parser->errorSymbol = parser->SyntaxErrorSymbol;
		if (parser->buffer != theFormat) {
			c_va_start(arguments, theFormat);
			c_vsnprintf(parser->buffer, parser->bufferSize, theFormat, arguments);
			c_va_end(arguments);
		}
		parser->errorMessage = fxNewParserString(parser, parser->buffer, mxStringLength(parser->buffer));
		c_longjmp(parser->firstJump->jmp_buf, 1);
	}
}

void fxTerminateParser(txParser* parser)
{
#if !defined(TARGET_PLAYDATE) && defined(PLUTO_NR_CAP_DIAG)
	static const char *bn[10] = {"<=16", "<=24", "<=32", "<=48", "<=64", "<=128", "<=256", "<=1K", "<=16K", "big"};
	txSize srcLen = 0;
	if (parser->source)
		srcLen = c_strlen(parser->source->string);
	fprintf(stderr, "[capdiag-parse] total=%luKB allocs=%lu avg=%.1fB src~%luKB\n",
		(unsigned long)(diagTotal >> 10), (unsigned long)diagCount,
		diagCount ? (double)diagTotal / (double)diagCount : 0.0,
		(unsigned long)(srcLen >> 10));
	for (int i = 0; i < 10; i++)
		if (diagAllocs[i])
			fprintf(stderr, "  [%s] n=%lu bytes=%luKB\n", bn[i],
				(unsigned long)diagAllocs[i], (unsigned long)(diagBytes[i] >> 10));
	diagTotal = 0; diagCount = 0;
	for (int i = 0; i < 10; i++) { diagAllocs[i] = 0; diagBytes[i] = 0; }
#endif
	fxDisposeParserChunks(parser);
}













