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

/* #define mxCodePrint 1 (diagnostic dump; re-enable for bytecode traces) */

#define mxByteCodePart\
	txByteCode* nextCode;\
	txInteger id;\
	txInteger stackLevel

#define kNoLine ((txUnsigned)~0)
	
struct sxByteCode {
	mxByteCodePart;
};
	
struct sxBigIntCode {
	mxByteCodePart;
	txBigInt bigint;
};
	
struct sxBranchCode {
	mxByteCodePart;
	txTargetCode* target;
};
	
struct sxIndexCode {
	mxByteCodePart;
	txInteger index;
};
	
struct sxIntegerCode {
	mxByteCodePart;
	txInteger integer;
};
	
struct sxNumberCode {
	mxByteCodePart;
	txNumber number;
};
	
struct sxStringCode {
	mxByteCodePart;
	txInteger length;
	txString string;
};
	
struct sxSymbolCode {
	mxByteCodePart;
	txSymbol* symbol;
};

struct sxTargetCode {
	mxByteCodePart;
	txInteger index;
	txLabelNode* label;
	txTargetCode* nextTarget;
	txInteger environmentLevel;
	txInteger scopeLevel;
	txInteger offset;
	txTargetCode* original;
	txBoolean used;
};
	
struct sxVariableCode {
	mxByteCodePart;
	txSymbol* symbol;
	txInteger index;
};

struct sxCoder {
	txParser* parser;
	txByteCode* firstCode;
	txByteCode* lastCode;
	txTargetCode* firstBreakTarget;
	txTargetCode* firstContinueTarget;
	txTargetCode* returnTarget;
	txInteger environmentLevel;
	txInteger scopeLevel;
	txInteger stackLevel;
	txInteger targetIndex;
	txSymbol* path;
	txUnsigned line;
	txBoolean programFlag;
	txBoolean evalFlag;
	txBoolean importFlag;
	txBoolean importMetaFlag;
	txClassNode* classNode;
	txTargetCode* chainTarget;
};

typedef struct {
	txInteger exception;
	txInteger selector;
	txTargetCode* catchTarget;
} txUsingContext;

typedef void (*txCompound)(void* it, void* param, txByte step);

static void fxCoderAdd(txCoder* self, txInteger delta, void* it);
static void fxCoderAddBigInt(txCoder* self, txInteger delta, txInteger id, txBigInt* bigint);
static void fxCoderAddBranch(txCoder* self, txInteger delta, txInteger id, txTargetCode* target);
static void fxCoderAddByte(txCoder* self, txInteger delta, txInteger id);
static void fxCoderAddIndex(txCoder* self, txInteger delta, txInteger id, txInteger index);
static void fxCoderAddInteger(txCoder* self, txInteger delta, txInteger id, txInteger integer);
static void fxCoderAddLine(txCoder* self, txInteger delta, txInteger id, txNode* node);
static void fxCoderAddNumber(txCoder* self, txInteger delta, txInteger id, txNumber number);
static void fxCoderAddString(txCoder* self, txInteger delta, txInteger id, txInteger length, txString string);
static void fxCoderAddSymbol(txCoder* self, txInteger delta, txInteger id, txSymbol* symbol);
static void fxCoderAddVariable(txCoder* self, txInteger delta, txInteger id, txSymbol* symbol, txInteger index);
static void fxCoderAdjustEnvironment(txCoder* self, txTargetCode* target);
static void fxCoderAdjustScope(txCoder* self, txTargetCode* target);
static txTargetCode* fxCoderAliasTargets(txCoder* self, txTargetCode* target);
static txInteger fxCoderCountParameters(txCoder* self, txNode* it);
static txTargetCode* fxCoderCreateTarget(txCoder* self);
static txTargetCode* fxCoderFinalizeTargets(txCoder* self, txTargetCode* alias, txInteger selector, txInteger* address, txTargetCode* finallyTarget);
static void fxCoderJumpTargets(txCoder* self, txTargetCode* target, txInteger selector, txInteger* address);
static void fxCoderOptimize(txCoder* self);
static txInteger fxCoderUseTemporaryVariable(txCoder* self);
static void fxCoderUnuseTemporaryVariables(txCoder* self, txInteger count);

static void fxScopeCoded(txScope* self, txCoder* coder);
static void fxScopeCodedBody(txScope* self, txCoder* coder);
static void fxScopeCodingBlock(txScope* self, txCoder* coder);
static void fxScopeCodingBody(txScope* self, txCoder* coder);
static void fxScopeCodingParams(txScope* self, txCoder* coder);
static void fxScopeCodingProgram(txScope* self, txCoder* coder);
static void fxScopeCodeDefineNodes(txScope* self, txCoder* coder);
static void fxScopeCodeRefresh(txScope* self, txCoder* coder);
static void fxScopeCodeReset(txScope* self, txCoder* coder);	static void fxScopeCodeRetrieve(txScope* self, txCoder* coder);
	/* xs_no_recursion (R7/B5c): forward decls for the WC_MODULE/WC_PROGRAM
	   machines (their definitions live later in this file). */
	static txInteger fxScopeCodeSpecifierNodes(txScope* self, txCoder* coder);
	static void fxScopeCodingEval(txScope* self, txCoder* coder);
	static void fxScopeCodeStore(txScope* self, txCoder* coder);
static void fxScopeCodeStoreAll(txScope* self, txCoder* coder);
static void fxScopeCodeUsed(txScope* self, txCoder* coder, txUsingContext* context);
static void fxScopeCodeUsedReverse(txScope* self, txCoder* coder, txDeclareNode* node, txInteger exception, txInteger selector);
static void fxScopeCodeUsing(txScope* self, txCoder* coder, txUsingContext* context);
static void fxScopeCodeUsingStatement(txScope* self, txCoder* coder, txNode* statement);

static void fxNodeDispatchCode(void* it, void* param);
static void fxNodeDispatchCodeAssign(void* it, void* param, txFlag flag);
static void fxNodeDispatchCodeDelete(void* it, void* param);
static void fxNodeDispatchCodeReference(void* it, void* param, txFlag flag);
static txFlag fxNodeDispatchCodeThis(void* it, void* param, txFlag flag);

/* xs_no_recursion: code-emitter walker. The tree dispatch tables still
   point at these functions; when the walker pump is running, the
   per-node functions become stubs that push a stage frame and return,
   and their real bodies run as stage machines in fxNodeCodeBody.
   Emission order is preserved exactly via the continuation frames. */
extern void fxNodeWalkPushCode(txParser* parser, void* it, void* param, int kind, int stage);
extern void fxNodeWalkPopCode(txParser* parser);
extern void fxNodeWalkDrainCode(txParser* parser, void* baseFrame);
extern void* fxNodeWalkStackTop(txParser* parser);
extern int fxNodeWalkRunningCheck(txParser* parser);
extern void fxNodeWalkCallCode(txParser* parser, void* it, void* param, int kind, int stage, txInteger i0, txInteger i1);
extern void fxNodeWalkSetRunningCode(txParser* parser, int value);

enum {
	WC_AND = 200, WC_ARRAY, WC_ARRAY_BINDING, WC_ARRAY_BINDING_ASSIGN, WC_ASSIGN, WC_AWAIT,
	WC_BINARY, WC_BINDING, WC_BINDING_ASSIGN, WC_BINDING_REFERENCE, WC_BLOCK, WC_BODY, WC_CALL, WC_CATCH, WC_CHAIN, WC_CHAIN_THIS,
	WC_CLASS, WC_COALESCE, WC_COMPOUND, WC_COMPOUND_NAME, WC_DECLARE, WC_DECLARE_ASSIGN, WC_DECLARE_REFERENCE, WC_DEFINE, WC_DELEGATE,
	WC_DELETE, WC_DO, WC_EXPRESSIONS, WC_EXPRESSIONS_DELETE, WC_EXPRESSIONS_THIS, WC_FIELD, WC_FOR, WC_FORINFOROF,
	WC_FUNCTION, WC_IF, WC_INCLUDE, WC_IMPORT_CALL, WC_LABEL, WC_MEMBER, WC_MEMBER_ASSIGN, WC_MEMBER_DELETE, WC_MEMBER_REFERENCE, WC_MEMBER_THIS, WC_MEMBER_AT,
	WC_MEMBER_AT_ASSIGN, WC_MEMBER_AT_DELETE, WC_MEMBER_AT_REFERENCE, WC_MEMBER_AT_THIS,
	WC_MODULE, WC_NEW, WC_OBJECT, WC_OBJECT_BINDING, WC_OBJECT_BINDING_ASSIGN, WC_OPTION, WC_OPTION_THIS, WC_OR,
	WC_PARAMS, WC_PARAMS_BINDING, WC_POSTFIX, WC_PRIVATE_IDENTIFIER, WC_PRIVATE_MEMBER,
	WC_PRIVATE_MEMBER_ASSIGN, WC_PRIVATE_MEMBER_DELETE, WC_PRIVATE_MEMBER_REFERENCE, WC_PRIVATE_MEMBER_THIS,
	WC_PROGRAM, WC_QUESTION, WC_REGEXP, WC_RETURN, WC_SPREAD, WC_STATEMENT,
	WC_STATEMENTS, WC_SUPER, WC_SWITCH, WC_TEMPLATE, WC_THROW, WC_TRY,	WC_UNARY, WC_USING_STMT, WC_WHILE, WC_WITH, WC_YIELD,
	WC_NATIVE, /* xs_no_recursion (R7): pump-context wrapper for unconverted subtrees */
	/* xs_no_recursion (R7/B2): pump-context wrappers for the Assign/Reference/
	   This dispatch variants of unconverted nodes. stage carries the flag. */
	WC_NASSIGN, WC_NREF, WC_NTHIS
};

static txFlag fxNodeCodeName(txNode* value);
static void fxCompoundExpressionNodeCodeName(void* it, void* param);
static void fxSpreadNodeCode(void* it, void* param, txInteger counter);

typedef struct sxCodeStep sxCodeStep;

struct sxCodeStep {
	void* fn;
	int kind;
};

/* One entry per (function, kind) whose native body was converted to a
   stage machine. fxNodeDispatchCode looks the callee up here: if it is
   converted it pushes a stage frame, otherwise it calls natively. */
static const sxCodeStep gxCodeSteps[] = {
	{(void*)fxAndExpressionNodeCode, WC_AND},
	/* xs_no_recursion (R7/B5a): ARRAY_BINDING/OBJECT_BINDING *_ASSIGN have
	   no machine yet — they must run as WC_NASSIGN wrappers, NOT fall
	   through into the WC_ASSIGN machine. Rows return when converted. */
	{(void*)fxArrayBindingNodeCodeAssign, WC_ARRAY_BINDING_ASSIGN},
	{(void*)fxArrayNodeCode, WC_ARRAY},
	{(void*)fxAssignNodeCode, WC_ASSIGN},
	{(void*)fxAwaitNodeCode, WC_AWAIT},
	{(void*)fxBinaryExpressionNodeCode, WC_BINARY},
	{(void*)fxBindingNodeCode, WC_BINDING},
	{(void*)fxBindingNodeCodeAssign, WC_BINDING_ASSIGN},
	{(void*)fxBindingNodeCodeReference, WC_BINDING_REFERENCE},
	{(void*)fxBlockNodeCode, WC_BLOCK},
	{(void*)fxBodyNodeCode, WC_BODY},
	{(void*)fxCallNodeCode, WC_CALL},
	{(void*)fxCatchNodeCode, WC_CATCH},
	{(void*)fxChainNodeCode, WC_CHAIN},
	{(void*)fxChainNodeCodeThis, WC_CHAIN_THIS},
	{(void*)fxClassNodeCode, WC_CLASS},
	{(void*)fxCoalesceExpressionNodeCode, WC_COALESCE},
	{(void*)fxCompoundExpressionNodeCode, WC_COMPOUND},
	{(void*)fxCompoundExpressionNodeCodeName, WC_COMPOUND_NAME},
	{(void*)fxDeclareNodeCode, WC_DECLARE},
	{(void*)fxDeclareNodeCodeAssign, WC_DECLARE_ASSIGN},
	{(void*)fxDeclareNodeCodeReference, WC_DECLARE_REFERENCE},
	{(void*)fxDefineNodeCode, WC_DEFINE},
	{(void*)fxDelegateNodeCode, WC_DELEGATE},
	{(void*)fxDeleteNodeCode, WC_DELETE},
	{(void*)fxDoNodeCode, WC_DO},
	{(void*)fxExpressionsNodeCode, WC_EXPRESSIONS},
	{(void*)fxExpressionsNodeCodeDelete, WC_EXPRESSIONS_DELETE},
	{(void*)fxExpressionsNodeCodeThis, WC_EXPRESSIONS_THIS},
	{(void*)fxFieldNodeCode, WC_FIELD},
	{(void*)fxForNodeCode, WC_FOR},
	{(void*)fxForInForOfNodeCode, WC_FORINFOROF},
	{(void*)fxFunctionNodeCode, WC_FUNCTION},
	{(void*)fxIfNodeCode, WC_IF},
	{(void*)fxIncludeNodeCode, WC_INCLUDE},
	{(void*)fxImportCallNodeCode, WC_IMPORT_CALL},
	{(void*)fxLabelNodeCode, WC_LABEL},
	{(void*)fxMemberAtNodeCode, WC_MEMBER_AT},
	{(void*)fxMemberAtNodeCodeAssign, WC_MEMBER_AT_ASSIGN},
	{(void*)fxMemberAtNodeCodeDelete, WC_MEMBER_AT_DELETE},
	{(void*)fxMemberAtNodeCodeReference, WC_MEMBER_AT_REFERENCE},
	{(void*)fxMemberAtNodeCodeThis, WC_MEMBER_AT_THIS},
	{(void*)fxMemberNodeCode, WC_MEMBER},
	{(void*)fxMemberNodeCodeAssign, WC_MEMBER_ASSIGN},
	{(void*)fxMemberNodeCodeDelete, WC_MEMBER_DELETE},
	{(void*)fxMemberNodeCodeReference, WC_MEMBER_REFERENCE},
	{(void*)fxMemberNodeCodeThis, WC_MEMBER_THIS},
	{(void*)fxModuleNodeCode, WC_MODULE},
	{(void*)fxNewNodeCode, WC_NEW},
	{(void*)fxObjectBindingNodeCodeAssign, WC_OBJECT_BINDING_ASSIGN},
	{(void*)fxObjectNodeCode, WC_OBJECT},
	{(void*)fxOptionNodeCode, WC_OPTION},
	{(void*)fxOptionNodeCodeThis, WC_OPTION_THIS},
	{(void*)fxOrExpressionNodeCode, WC_OR},
	{(void*)fxParamsBindingNodeCode, WC_PARAMS_BINDING},
	{(void*)fxParamsNodeCode, WC_PARAMS},
	{(void*)fxPostfixExpressionNodeCode, WC_POSTFIX},
	{(void*)fxPrivateIdentifierNodeCode, WC_PRIVATE_IDENTIFIER},
	{(void*)fxPrivateMemberNodeCode, WC_PRIVATE_MEMBER},
	{(void*)fxPrivateMemberNodeCodeAssign, WC_PRIVATE_MEMBER_ASSIGN},
	{(void*)fxPrivateMemberNodeCodeDelete, WC_PRIVATE_MEMBER_DELETE},
	{(void*)fxPrivateMemberNodeCodeReference, WC_PRIVATE_MEMBER_REFERENCE},
	{(void*)fxPrivateMemberNodeCodeThis, WC_PRIVATE_MEMBER_THIS},
	{(void*)fxProgramNodeCode, WC_PROGRAM},
	{(void*)fxQuestionMarkNodeCode, WC_QUESTION},
	{(void*)fxRegexpNodeCode, WC_REGEXP},
	{(void*)fxReturnNodeCode, WC_RETURN},
	{(void*)fxSpreadNodeCode, WC_SPREAD},
	{(void*)fxStatementNodeCode, WC_STATEMENT},
	{(void*)fxStatementsNodeCode, WC_STATEMENTS},
	{(void*)fxSuperNodeCode, WC_SUPER},
	{(void*)fxSwitchNodeCode, WC_SWITCH},
	{(void*)fxTemplateNodeCode, WC_TEMPLATE},
	{(void*)fxThrowNodeCode, WC_THROW},
	{(void*)fxTryNodeCode, WC_TRY},
	{(void*)fxUnaryExpressionNodeCode, WC_UNARY},
	{(void*)fxWhileNodeCode, WC_WHILE},
	{(void*)fxWithNodeCode, WC_WITH},
	{(void*)fxYieldNodeCode, WC_YIELD},
	{C_NULL, 0}
};

/* xs_no_recursion (R7): mixed-mode gate. Only kinds whose stage machines
   are written in fxNodeCodeBody are routed through the walker pump; every
   other node runs through its native emitter exactly like stock (still
   bounded by fxCheckParserStack). Add a kind to the converted set below
   when its machine lands in fxNodeCodeBody. */
static int fxCodeStepKindFor(void* fn)
{
	static const int converted[] = {
		WC_MEMBER,
		WC_MEMBER_ASSIGN,
		WC_MEMBER_DELETE,
		WC_MEMBER_REFERENCE,
		WC_PRIVATE_IDENTIFIER,
		WC_THROW,
		WC_UNARY,
		/* R7/B2 */
		WC_ASSIGN,
		WC_BINARY,
		WC_QUESTION,
		WC_AND,
		WC_OR,
		WC_COALESCE,
		WC_COMPOUND,
		WC_POSTFIX,
		WC_EXPRESSIONS,
		WC_EXPRESSIONS_THIS,
		WC_EXPRESSIONS_DELETE,
		/* R7/B4a */
		WC_BLOCK,
		WC_BODY,
		WC_STATEMENTS,
		WC_STATEMENT,
		WC_LABEL,
		WC_WHILE,
		WC_DO,
		WC_IF,
		WC_WITH,
		WC_RETURN,
		/* R7/B4b */
		WC_FOR,
		WC_FORINFOROF,
		WC_SWITCH,
		WC_CATCH,
		WC_TRY,
		WC_AWAIT,
		WC_YIELD,
		WC_DELETE,
		/* R7/B3 */
		WC_CALL,
		WC_NEW,
		WC_CHAIN,
		WC_CHAIN_THIS,
		WC_OPTION,
		WC_OPTION_THIS,
		WC_MEMBER_AT,
		WC_MEMBER_AT_DELETE,
		WC_MEMBER_AT_REFERENCE,
		WC_PRIVATE_MEMBER,
		WC_PRIVATE_MEMBER_ASSIGN,
		WC_PRIVATE_MEMBER_DELETE,
		WC_PRIVATE_MEMBER_REFERENCE,
		WC_TEMPLATE,
		WC_SPREAD,
		/* R7/B5a */
		WC_DECLARE,
		WC_DECLARE_ASSIGN,
		WC_DECLARE_REFERENCE,
		WC_DEFINE,
		WC_FIELD,
		WC_PARAMS,
		WC_OBJECT,
		WC_ARRAY,
		WC_FUNCTION,
		/* R7/B5b */
		WC_CLASS,
		/* R7/B5c */
		WC_INCLUDE,
		WC_IMPORT_CALL,
		WC_MODULE,
		WC_PROGRAM,
		/* R7/B5d: final sweep — everything that used to run native */
		WC_BINDING,
		WC_BINDING_ASSIGN,
		WC_BINDING_REFERENCE,
		WC_COMPOUND_NAME,
		WC_MEMBER_AT_ASSIGN,
		WC_MEMBER_AT_THIS,
		WC_MEMBER_THIS,
		WC_OBJECT_BINDING_ASSIGN,
		WC_PARAMS_BINDING,
		WC_PRIVATE_MEMBER_THIS,
		WC_REGEXP,
		WC_SUPER,
		WC_DELEGATE,
		WC_ARRAY_BINDING_ASSIGN,
		0
	};
	const sxCodeStep* step;
	const int* k;
	step = gxCodeSteps;
	while (step->fn) {
		if (step->fn == fn) {
			for (k = converted; *k; k++)
				if (*k == step->kind)
					return step->kind;
			return 0;
		}
		step++;
	}
	return 0;
}

/* xs_no_recursion (R7): code-emission stage machine. Each frame is one
   node visit: stage 0 runs the emitter's prologue (child dispatches push
   child frames and return through the pump), stage 1 runs the epilogue
   (the fxCoderAdd* after the recursive call in stock). frame->extra holds
   the txFlag argument of the code* dispatch variants across the two
   stages (fxNodeWalkPush does not clear it; stage 0 always initializes
   it). A frame whose stage 0 pushed children returns without popping;
   the pump calls it again at stage 1 once the children are done. */
void fxNodeCodeBody(txParser* parser)
{
	txNodeWalkFrame* frame = parser->nodeWalkStack;
	txNode* self = (txNode*)frame->it;
	txCoder* coder = (txCoder*)frame->param;
	switch (frame->kind) {
	case WC_THROW:
		if (frame->stage == 0) {
			txStatementNode* node = (txStatementNode*)self;
			frame->stage = 1;
			fxNodeDispatchCode(node->expression, coder);
			if (parser->nodeWalkStack != frame)
				return;
		}
		fxCoderAddByte(coder, -1, XS_CODE_THROW);
		fxNodeWalkPopCode(parser);
		return;
	case WC_UNARY:
		if (frame->stage == 0) {
			txUnaryExpressionNode* node = (txUnaryExpressionNode*)self;
			frame->stage = 1;
			fxNodeDispatchCode(node->right, coder);
			if (parser->nodeWalkStack != frame)
				return;
		}
		fxCoderAddByte(coder, 0, self->description->code);
		fxNodeWalkPopCode(parser);
		return;
	case WC_PRIVATE_IDENTIFIER:
		if (frame->stage == 0) {
			txPrivateMemberNode* node = (txPrivateMemberNode*)self;
			frame->stage = 1;
			fxNodeDispatchCode(node->reference, coder);
			if (parser->nodeWalkStack != frame)
				return;
		}
		fxCoderAddIndex(coder, 0, XS_CODE_HAS_PRIVATE_1, ((txPrivateMemberNode*)self)->declaration->index);
		fxNodeWalkPopCode(parser);
		return;
	case WC_MEMBER:
		if (frame->stage == 0) {
			txMemberNode* node = (txMemberNode*)self;
			frame->stage = 1;
			fxNodeDispatchCode(node->reference, coder);
			if (parser->nodeWalkStack != frame)
				return;
		}
		fxCoderAddSymbol(coder, 0, (self->flags & mxSuperFlag) ? XS_CODE_GET_SUPER : XS_CODE_GET_PROPERTY, ((txMemberNode*)self)->symbol);
		fxNodeWalkPopCode(parser);
		return;
	case WC_MEMBER_DELETE:
		if (frame->stage == 0) {
			txMemberNode* node = (txMemberNode*)self;
			frame->stage = 1;
			fxNodeDispatchCode(node->reference, coder);
			if (parser->nodeWalkStack != frame)
				return;
		}
		fxCoderAddSymbol(coder, 0, (self->flags & mxSuperFlag) ? XS_CODE_DELETE_SUPER : XS_CODE_DELETE_PROPERTY, ((txMemberNode*)self)->symbol);
		fxNodeWalkPopCode(parser);
		return;
	case WC_MEMBER_REFERENCE:
		/* xs_no_recursion (R7): the Reference dispatch passes its flag as
		   frame->stage, so this kind tracks its own phase in i0 (cleared by
		   fxNodeWalkPush). Stock ignores the flag here and just dispatches
		   the reference. */
		if (frame->i0 == 0) {
			txMemberNode* node = (txMemberNode*)self;
			frame->i0 = 1;
			fxNodeDispatchCode(node->reference, coder);
			if (parser->nodeWalkStack != frame)
				return;
		}
		fxNodeWalkPopCode(parser);
		return;
	case WC_MEMBER_ASSIGN:
		fxCoderAddSymbol(coder, -1, (((txMemberNode*)self)->reference->flags & mxSuperFlag) ? XS_CODE_SET_SUPER : XS_CODE_SET_PROPERTY, ((txMemberNode*)self)->symbol);
		fxNodeWalkPopCode(parser);
		return;
	/* xs_no_recursion (R7/B2): statement-sequenced emitters. stage doubles
	   as the phase cursor; v0/v1 hold loop cursors and scratch targets. */
	case WC_ASSIGN:
		/* stock: fxAssignNodeCode = Reference(flag 1); value; Assign(flag 1). */
		switch (frame->stage) {
		case 0:
			frame->stage = 1;
			fxNodeDispatchCodeReference(((txAssignNode*)self)->reference, coder, 1);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 1:
			frame->stage = 2;
			fxNodeDispatchCode(((txAssignNode*)self)->value, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 2:
			frame->stage = 1; /* pop own frame BEFORE the epilogue dispatch */
			fxNodeWalkPopCode(parser);
			fxNodeDispatchCodeAssign(((txAssignNode*)self)->reference, coder, 1);
			return;
		default:
			fxReportParserError(parser, self ? self->line : 0, "xs_no_recursion: bad WC_ASSIGN stage");
			fxNodeWalkPopCode(parser);
			return;
		}
	case WC_BINARY:
		/* stock: left, right, op. Switch shape: two dispatches must be
		   staged across revisits, not nested in one if-block. */
		switch (frame->stage) {
		case 0:
			frame->stage = 1;
			fxNodeDispatchCode(((txBinaryExpressionNode*)self)->left, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 1:
			frame->stage = 2;
			fxNodeDispatchCode(((txBinaryExpressionNode*)self)->right, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		default:
			fxCoderAddByte(coder, -1, self->description->code);
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_QUESTION: {
			txQuestionMarkNode* node = (txQuestionMarkNode*)self;
			node->thenExpression->flags |= (self->flags & mxTailRecursionFlag);
			node->elseExpression->flags |= (self->flags & mxTailRecursionFlag);
			switch (frame->stage) {
			case 0:
				frame->v0 = fxCoderCreateTarget(coder);
				frame->v1 = fxCoderCreateTarget(coder);
				frame->stage = 1;
				fxNodeDispatchCode(node->expression, coder);
				if (parser->nodeWalkStack != frame)
					return;
				/* fall through */
			case 1:
				fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_ELSE_1, (txTargetCode*)frame->v0);
				frame->stage = 2;
				fxNodeDispatchCode(node->thenExpression, coder);
				if (parser->nodeWalkStack != frame)
					return;
				/* fall through */
			case 2:
				fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, (txTargetCode*)frame->v1);
				fxCoderAdd(coder, -1, (txTargetCode*)frame->v0);
				frame->stage = 3;
				fxNodeDispatchCode(node->elseExpression, coder);
				if (parser->nodeWalkStack != frame)
					return;
				/* fall through */
			default:
				fxCoderAdd(coder, 0, (txTargetCode*)frame->v1);
				fxNodeWalkPopCode(parser);
			}
		}
		return;
	case WC_AND:
	case WC_OR:
	case WC_COALESCE: {
			txBinaryExpressionNode* node = (txBinaryExpressionNode*)self;
			node->right->flags |= (self->flags & mxTailRecursionFlag);
			switch (frame->stage) {
			case 0:
				frame->v0 = fxCoderCreateTarget(coder);
				frame->stage = 1;
				fxNodeDispatchCode(node->left, coder);
				if (parser->nodeWalkStack != frame)
					return;
				/* fall through */
			case 1:
				switch (frame->kind) {
				case WC_AND:
					fxCoderAddByte(coder, 1, XS_CODE_DUB);
					fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_ELSE_1, (txTargetCode*)frame->v0);
					fxCoderAddByte(coder, -1, XS_CODE_POP);
					break;
				case WC_OR:
					fxCoderAddByte(coder, 1, XS_CODE_DUB);
					fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, (txTargetCode*)frame->v0);
					fxCoderAddByte(coder, -1, XS_CODE_POP);
					break;
				default:
					fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_COALESCE_1, (txTargetCode*)frame->v0);
					break;
				}
				frame->stage = 2;
				fxNodeDispatchCode(node->right, coder);
				if (parser->nodeWalkStack != frame)
					return;
				/* fall through */
			default:
				fxCoderAdd(coder, 0, (txTargetCode*)frame->v0);
				fxNodeWalkPopCode(parser);
			}
		}
		return;
	case WC_COMPOUND: {
			/* stock: fxCompoundExpressionNodeCode. Stage layout:
			   0 prologue+This, 1 body(value+name), 2 Assign, 3 shortcut tail. */
			txAssignNode* node = (txAssignNode*)self;
			txCoder* c = coder;
			txToken token = self->description->token;
			txFlag shortcut = ((token == XS_TOKEN_AND_ASSIGN) || (token == XS_TOKEN_COALESCE_ASSIGN) || (token == XS_TOKEN_OR_ASSIGN)) ? 1 : 0;
			txInteger stackLevel;
			(void)stackLevel;
			txFlag swap;
			switch (frame->stage) {
			case 0:
				frame->v0 = (shortcut) ? fxCoderCreateTarget(c) : C_NULL;		/* elseTarget */
				frame->v1 = (shortcut) ? fxCoderCreateTarget(c) : C_NULL;		/* endTarget */
				frame->stage = 1;
				fxNodeDispatchCodeThis(node->reference, c, 1);
				/* xs_no_recursion (R7/B3): the swap count is the child This
				   result. When the child pushed, its value was staged in
				   parser->codeThisResult (read after the drain, before the
				   revisit); when it completed inline, the wrapper returned it
				   there too. */
				frame->i0 = parser->codeThisResult;
				if (parser->nodeWalkStack != frame)
					return;
				/* fall through */
			case 1:
				switch (token) {
				case XS_TOKEN_AND_ASSIGN:
					fxCoderAddByte(c, 1, XS_CODE_DUB);
					fxCoderAddBranch(c, -1, XS_CODE_BRANCH_ELSE_1, (txTargetCode*)frame->v0);
					fxCoderAddByte(c, -1, XS_CODE_POP);
					break;
				case XS_TOKEN_COALESCE_ASSIGN:
					fxCoderAddBranch(c, -1, XS_CODE_BRANCH_COALESCE_1, (txTargetCode*)frame->v0);
					break;
				case XS_TOKEN_OR_ASSIGN:
					fxCoderAddByte(c, 1, XS_CODE_DUB);
					fxCoderAddBranch(c, -1, XS_CODE_BRANCH_IF_1, (txTargetCode*)frame->v0);
					fxCoderAddByte(c, -1, XS_CODE_POP);
					break;
				}
				frame->stage = 2;
				fxNodeDispatchCode(node->value, c);
				if (parser->nodeWalkStack != frame)
					return;
				/* fall through */
			case 2:
				if (shortcut) {
					fxCompoundExpressionNodeCodeName(self, c);
					frame->i1 = c->stackLevel;	/* stackLevel after body (shortcut only) */
				}
				else
					fxCoderAddByte(c, -1, self->description->code);
				frame->stage = 3;
				fxNodeDispatchCodeAssign(node->reference, c, 0);
				if (parser->nodeWalkStack != frame)
					return;
				/* fall through */
			default:
				if (!shortcut) {
					fxNodeWalkPopCode(parser);
					return;
				}
				fxCoderAddBranch(c, 0, XS_CODE_BRANCH_1, (txTargetCode*)frame->v1);
				c->stackLevel = (txInteger)frame->i1;
				fxCoderAdd(c, 0, (txTargetCode*)frame->v0);
				for (swap = (txFlag)frame->i0; swap > 0; swap--) {
					if (!(self->flags & mxExpressionNoValue))
						fxCoderAddByte(c, 0, XS_CODE_SWAP);
					fxCoderAddByte(c, -1, XS_CODE_POP);
				}
				fxCoderAdd(c, 0, (txTargetCode*)frame->v1);
				fxNodeWalkPopCode(parser);
			}
		}
		return;
	case WC_POSTFIX: {
			/* stock: fxPostfixExpressionNodeCode. i0 = value temp, i1 = noValue. */
			txPostfixExpressionNode* node = (txPostfixExpressionNode*)self;
			switch (frame->stage) {
			case 0:
				frame->i1 = !(self->flags & mxExpressionNoValue);
				frame->stage = 1;
				fxNodeDispatchCodeThis(node->left, coder, 1);
				if (parser->nodeWalkStack != frame)
					return;
				/* fall through */
			case 1:
				if (frame->i1) {
					frame->i0 = fxCoderUseTemporaryVariable(coder);
					fxCoderAddByte(coder, 0, XS_CODE_TO_NUMERIC);
					fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->i0);
				}
				fxCoderAddByte(coder, 0, self->description->code);
				frame->stage = 2;
				fxNodeDispatchCodeAssign(node->left, coder, 0);
				if (parser->nodeWalkStack != frame)
					return;
				/* fall through */
			case 2:
				if (frame->i1) {
					fxCoderAddByte(coder, -1, XS_CODE_POP);
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i0);
					fxCoderUnuseTemporaryVariables(coder, 1);
				}
				fxNodeWalkPopCode(parser);
				return;
			}
		}
		return;
	case WC_EXPRESSIONS: {
			/* stock: fxExpressionsNodeCode. v0 = current item,
			   i0 = "previous" flag (emit POP before subsequent items).
			   The cursor advances BEFORE the dispatch: a pushed child pops
			   back into this frame at stage 1, and the while must resume at
			   the next item, not re-run the completed one. */
			txExpressionsNode* node = (txExpressionsNode*)self;
			if (node->items) {
				if (frame->stage == 0) {
					frame->v0 = node->items->first;
					frame->i0 = 0;
					frame->stage = 1;
				}
				while (frame->v0) {
					txNode* item = (txNode*)frame->v0;
					txNode* next = item->next;
					if (!next)
						item->flags |= (self->flags & mxTailRecursionFlag);
					if (frame->i0)
						fxCoderAddByte(coder, -1, XS_CODE_POP);
					frame->v0 = next;
					/* stock sets `previous` after the dispatch returns; set it
					   before instead so a parked child cannot lose it (the
					   flag is only read at the top of the next iteration) */
					frame->i0 = 1;
					fxNodeDispatchCode(item, coder);
					if (parser->nodeWalkStack != frame)
						return;
				}
			}
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_EXPRESSIONS_THIS:
		/* stock: fxExpressionsNodeCodeThis — single item delegates to the
		   item's This; otherwise the generic undefined+dispatch form. */
		switch (frame->stage) {
		case 0: {
				txExpressionsNode* node = (txExpressionsNode*)self;
				txNode* item = node->items ? node->items->first : C_NULL;
				frame->i2 = frame->stage;	/* incoming flag (stage doubles as flag) */
				frame->i1 = (item && !item->next) ? 1 : 0;
				if (frame->i1)
					frame->v0 = item;
				frame->stage = 1;
				if (frame->i1)
					parser->codeThisResult = fxNodeDispatchCodeThis((txNode*)frame->v0, coder, (txFlag)frame->i2);
				else {
					fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
					fxNodeDispatchCode(self, coder);
				}
				if (parser->nodeWalkStack != frame)
					return;
				if (frame->i1)
					goto wce_this_done;
				frame->stage = 2;
				goto wce_this_generic;
			}
		case 1:
		wce_this_done:
			parser->codeThisResult = frame->i1 ? (txInteger)parser->codeThisResult : 1;
			fxNodeWalkPopCode(parser);
			return;
		case 2:
		wce_this_generic:
			parser->codeThisResult = 1;
			fxNodeWalkPopCode(parser);
			return;
		}
		return;
	case WC_EXPRESSIONS_DELETE: {
			/* stock: fxExpressionsNodeCodeDelete — single item delegates to
			   the item's Delete; otherwise the generic POP/TRUE form. */
			txExpressionsNode* node = (txExpressionsNode*)self;
			txNode* item = node->items ? node->items->first : C_NULL;
			if (item && !item->next) {
				if (frame->stage == 0) {
					frame->stage = 1;
					fxNodeDispatchCodeDelete(item, coder);
					if (parser->nodeWalkStack != frame)
						return;
				}
			}
			else {
				if (frame->stage == 0) {
					frame->stage = 1;
					fxNodeDispatchCode(self, coder);
					if (parser->nodeWalkStack != frame)
						return;
					fxCoderAddByte(coder, -1, XS_CODE_POP);
					fxCoderAddByte(coder, 1, XS_CODE_TRUE);
				}
			}
			fxNodeWalkPopCode(parser);
		}
		return;
	/* xs_no_recursion (R7/B4a): statements. Target chains
	   (firstBreak/firstContinue/returnTarget) are machine-global state
	   mutated synchronously around dispatches — every converted emitter
	   restores them before its pop. The using-context lives in frame
	   fields (i0=exception, i1=selector, v2=catchTarget) across stages. */
	case WC_BLOCK:
		/* stock: fxBlockNodeCode. */
		switch (frame->i0 >= 0x1000 ? 2 : frame->stage) {
		case 0:
			fxScopeCodingBlock(((txBlockNode*)self)->scope, coder);
			/* xs_no_recursion (R7/B5c): plain-code helper run from a machine
			   stage: clear running so the define protocol's dispatches
			   complete inline (nested Call drains). With running=1 the
			   pushed define frames would park and resume after this frame,
			   reordering closures relative to stock. */
			{
				int running_nr = parser->nodeWalkRunning;
				parser->nodeWalkRunning = 0;
				fxScopeCodeDefineNodes(((txBlockNode*)self)->scope, coder);
				parser->nodeWalkRunning = running_nr;
			}
			if (((txBlockNode*)self)->scope->disposableNodeCount) {
				txUsingContext context;
				fxScopeCodeUsing(((txBlockNode*)self)->scope, coder, &context);
				frame->i0 = 0x1000 | context.exception;
				frame->i1 = context.selector;
				frame->v2 = context.catchTarget;
			}
			frame->stage = 1;
			fxNodeDispatchCode(((txBlockNode*)self)->statement, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		default:
			if (frame->i0 >= 0x1000) {
				txUsingContext context;
				context.exception = (frame->i0 & 0xFFF);
				context.selector = (txInteger)frame->i1;
				context.catchTarget = (txTargetCode*)frame->v2;
				fxScopeCodeUsed(((txBlockNode*)self)->scope, coder, &context);
			}
			fxScopeCoded(((txBlockNode*)self)->scope, coder);
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_BODY:
		/* stock: fxBodyNodeCode — evalFlag save/set around the body scope. */
		switch (frame->stage) {
		case 0: {
				txCoder* c = coder;
				txBoolean evalFlag = c->evalFlag;
				if (((txBlockNode*)self)->flags & mxEvalFlag && !(((txBlockNode*)self)->flags & mxStrictFlag))
					c->evalFlag = 1;
				frame->i1 = evalFlag;
				fxScopeCodingBody(((txBlockNode*)self)->scope, c);
				/* xs_no_recursion (R7/B5c): see running-clear note above. */
				{
					int running_nr = parser->nodeWalkRunning;
					parser->nodeWalkRunning = 0;
					fxScopeCodeDefineNodes(((txBlockNode*)self)->scope, c);
					parser->nodeWalkRunning = running_nr;
				}
				if (((txBlockNode*)self)->scope->disposableNodeCount) {
					txUsingContext context;
					fxScopeCodeUsing(((txBlockNode*)self)->scope, c, &context);
					frame->i0 = 0x1000 | context.exception;
					frame->v2 = context.catchTarget;
					frame->i2 = context.selector;
				}
				frame->stage = 1;
				fxNodeDispatchCode(((txBlockNode*)self)->statement, c);
				if (parser->nodeWalkStack != frame)
					return;
			}
			/* fall through */
		default: {
				txCoder* c = coder;
				if (frame->i0 >= 0x1000) {
					txUsingContext context;
					context.exception = (frame->i0 & 0xFFF);
					context.selector = (txInteger)frame->i2;
					context.catchTarget = (txTargetCode*)frame->v2;
					fxScopeCodeUsed(((txBlockNode*)self)->scope, c, &context);
				}
				fxScopeCodedBody(((txBlockNode*)self)->scope, c);
				if (((txBlockNode*)self)->flags & mxEvalFlag && !(((txBlockNode*)self)->flags & mxStrictFlag))
					c->evalFlag = (txBoolean)frame->i1;
				fxNodeWalkPopCode(parser);
			}
		}
		return;
	case WC_STATEMENTS: {
			/* stock: fxStatementsNodeCode — plain statement list. */
			txStatementsNode* node = (txStatementsNode*)self;
			if (frame->stage == 0) {
				frame->v0 = node->items->first;
				frame->stage = 1;
			}
			while (frame->v0) {
				txNode* item = (txNode*)frame->v0;
				frame->v0 = item->next;
				frame->i6 = coder->scopeLevel;
				frame->i7 = item->description->token;
				fxNodeDispatchCode(item, coder);
				if (parser->nodeWalkStack != frame)
					return;
			}
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_STATEMENT:
		/* stock: fxStatementNodeCode — value vs no-value statement
		   expression, with the SET_CLOSURE/SET_LOCAL to PULL patch after
		   the child completes (the revisit sees the final lastCode). */
		switch (frame->stage) {
		case 0:
			frame->stage = 1;
			if (coder->programFlag) {
				fxNodeDispatchCode(((txStatementNode*)self)->expression, coder);
				if (parser->nodeWalkStack != frame)
					return;
			}
			else {
				((txStatementNode*)self)->expression->flags |= mxExpressionNoValue;
				fxNodeDispatchCode(((txStatementNode*)self)->expression, coder);
				if (parser->nodeWalkStack != frame)
					return;
			}
			/* fall through */
		default:
			if (coder->programFlag)
				fxCoderAddByte(coder, -1, XS_CODE_SET_RESULT);
			else {
				if (coder->lastCode->id == XS_CODE_SET_CLOSURE_1) {
					coder->lastCode->id = XS_CODE_PULL_CLOSURE_1;
					coder->stackLevel--;
					coder->lastCode->stackLevel = coder->stackLevel;
				}
				else if (coder->lastCode->id == XS_CODE_SET_LOCAL_1) {
					coder->lastCode->id = XS_CODE_PULL_LOCAL_1;
					coder->stackLevel--;
					coder->lastCode->stackLevel = coder->stackLevel;
				}
				else
					fxCoderAddByte(coder, -1, XS_CODE_POP);
			}
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_LABEL:
		/* stock: fxLabelNodeCode — label-chain checks (native), install
		   break target (and continue target when unlabeled), dispatch the
		   statement, place/close the targets. v0 = breakTarget,
		   v1 = continueTarget, i0 = unlabeled flag.
		   NOTE: the dup-check walks assume stock's nextLabel chain is
		   well-formed; if the parser ever produces a cycle the walk must
		   still terminate (stock would spin here too, but stock never
		   reaches this with a cycle because the parser's fxPushNodeStruct
		   order differs). Cap the walk defensively. */
		switch (frame->stage) {
		case 0: {
				txLabelNode* node = (txLabelNode*)self;
				txNode* statement = node->statement;
				txTargetCode* breakTarget;
				/* xs_no_recursion (R12 fix): stock REBINDS self to each
				   successive label while building the chain (former->nextLabel
				   = self; self = former). Wiring every link back to the fixed
				   outermost node destroys the intermediate links, so a labeled
				   `continue` aimed at an inner label of a multi-label loop
				   (React's `e:t:for` pattern) fails label lookup — the bundle
				   probe caught exactly that (chain printed anon>e instead of
				   anon>t>e). Emulate stock's advance-rewire with a local cur. */
				txLabelNode* cur = node;
				while (statement->description->token == XS_TOKEN_LABEL) {
					txLabelNode* former = (txLabelNode*)statement;
					txLabelNode* current = cur;
					txInteger guard_nr = 0;
					while (current && (guard_nr++ < 64)) {
						if (former->symbol && current->symbol && (former->symbol == current->symbol))
							fxReportParserError(coder->parser, current->line, "duplicate label %s", current->symbol->string);
						current = current->nextLabel;
					}
					former->nextLabel = cur;
					cur = former;
					statement = former->statement;
				}
				frame->v3 = cur;
				breakTarget = coder->firstBreakTarget;
				while (breakTarget) {
					txLabelNode* former = breakTarget->label;
					if (former) {
						txLabelNode* current = (txLabelNode*)(frame->v3 ? frame->v3 : node);
						txInteger guard = 0;
						while (current && (guard++ < 64)) {
							if (former->symbol && current->symbol && (former->symbol == current->symbol))
								fxReportParserError(coder->parser, current->line, "duplicate label %s", current->symbol->string);
							current = current->nextLabel;
						}
					}
					breakTarget = breakTarget->nextTarget;
				}
				breakTarget = fxCoderCreateTarget(coder);
				breakTarget->nextTarget = coder->firstBreakTarget;
				coder->firstBreakTarget = breakTarget;
				/* xs_no_recursion (R7/B4): the label-chain loop rebinds
				   "self" to the INNERMOST label (the parser always wraps
				   loops/with in an anonymous LABEL, symbol == NULL); its
				   symbol decides the continue-target install and its node
				   is the target's label. Emulate with v3 = innermost. */
				breakTarget->label = (txLabelNode*)(frame->v3 ? frame->v3 : node);
				frame->v0 = breakTarget;
				if (((txLabelNode*)(frame->v3 ? frame->v3 : node))->symbol)
					frame->i0 = 0;
				else {
					txTargetCode* continueTarget = fxCoderCreateTarget(coder);
					continueTarget->nextTarget = coder->firstContinueTarget;
					coder->firstContinueTarget = continueTarget;
					continueTarget->label = (txLabelNode*)(frame->v3 ? frame->v3 : node);
					frame->v1 = continueTarget;
					frame->i0 = 1;
				}
				frame->stage = 1;
				fxNodeDispatchCode(statement, coder);
				if (parser->nodeWalkStack != frame)
					return;
			}
			/* fall through */
		default:
			if (frame->i0)
				coder->firstContinueTarget = ((txTargetCode*)frame->v1)->nextTarget;
			fxCoderAdd(coder, 0, coder->firstBreakTarget);
			coder->firstBreakTarget = ((txTargetCode*)frame->v0)->nextTarget;
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_WHILE:
		/* stock: fxWhileNodeCode — targets come pre-installed by the
		   enclosing label node. */
		switch (frame->stage) {
		case 0:
			frame->stage = 1;
			if (coder->programFlag) {
				fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
				fxCoderAddByte(coder, -1, XS_CODE_SET_RESULT);
			}
			fxCoderAdd(coder, 0, coder->firstContinueTarget);
			fxNodeDispatchCode(((txWhileNode*)self)->expression, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 1:
			fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_ELSE_1, coder->firstBreakTarget);
			frame->stage = 2;
			fxNodeDispatchCode(((txWhileNode*)self)->statement, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		default:
			fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, coder->firstContinueTarget);
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_DO:
		/* stock: fxDoNodeCode. v0 = loopTarget. */
		switch (frame->stage) {
		case 0:
			frame->v0 = fxCoderCreateTarget(coder);
			if (coder->programFlag) {
				fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
				fxCoderAddByte(coder, -1, XS_CODE_SET_RESULT);
			}
			fxCoderAdd(coder, 0, (txTargetCode*)frame->v0);
			frame->stage = 1;
			fxNodeDispatchCode(((txDoNode*)self)->statement, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 1:
			fxCoderAdd(coder, 0, coder->firstContinueTarget);
			frame->stage = 2;
			fxNodeDispatchCode(((txDoNode*)self)->expression, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		default:
			fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, (txTargetCode*)frame->v0);
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_IF:
		/* stock: fxIfNodeCode — three shapes by programFlag/else.
		   stage: 0 expr, 1 then, 2 else-or-close, 4 close-after-else. */
		switch (frame->stage) {
		case 0:
			frame->stage = 1;
			fxNodeDispatchCode(((txIfNode*)self)->expression, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 1:
			if (coder->programFlag) {
				frame->v0 = fxCoderCreateTarget(coder);
				frame->v1 = fxCoderCreateTarget(coder);
				fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_ELSE_1, (txTargetCode*)frame->v0);
				fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
				fxCoderAddByte(coder, -1, XS_CODE_SET_RESULT);
				frame->i0 = 1;	/* programFlag shape */
			}
			else if (((txIfNode*)self)->elseStatement) {
				frame->v0 = fxCoderCreateTarget(coder);
				frame->v1 = fxCoderCreateTarget(coder);
				fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_ELSE_1, (txTargetCode*)frame->v0);
				frame->i0 = 2;	/* else shape */
			}
			else {
				frame->v1 = fxCoderCreateTarget(coder);
				fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_ELSE_1, (txTargetCode*)frame->v1);
				frame->i0 = 3;	/* bare shape: only endTarget */
			}
			frame->stage = 2;
			fxNodeDispatchCode(((txIfNode*)self)->thenStatement, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 2:
			if (frame->i0 == 1) {
				fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, (txTargetCode*)frame->v1);
				fxCoderAdd(coder, 0, (txTargetCode*)frame->v0);
				fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
				fxCoderAddByte(coder, -1, XS_CODE_SET_RESULT);
			}
			else if (frame->i0 == 2) {
				fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, (txTargetCode*)frame->v1);
				fxCoderAdd(coder, 0, (txTargetCode*)frame->v0);
			}
			if (frame->i0 == 3) {
				fxCoderAdd(coder, 0, (txTargetCode*)frame->v1);
				fxNodeWalkPopCode(parser);
				return;
			}
			/* xs_no_recursion (R7/B4): stock guards the else dispatch —
			   elseStatement is C_NULL when the if has no else. */
			if (((txIfNode*)self)->elseStatement) {
				frame->stage = 4;
				fxNodeDispatchCode(((txIfNode*)self)->elseStatement, coder);
				if (parser->nodeWalkStack != frame)
					return;
			}
			/* fall through */
		default:
			fxCoderAdd(coder, 0, (txTargetCode*)frame->v1);
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_WITH:
		/* stock: fxWithNodeCode. */
		switch (frame->stage) {
		case 0:
			frame->stage = 1;
			fxNodeDispatchCode(((txWithNode*)self)->expression, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 1:
			fxCoderAddByte(coder, 0, XS_CODE_TO_INSTANCE);
			fxCoderAddByte(coder, 0, XS_CODE_WITH);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			frame->i0 = coder->evalFlag;
			coder->environmentLevel++;
			coder->evalFlag = 1;
			if (coder->programFlag) {
				fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
				fxCoderAddByte(coder, -1, XS_CODE_SET_RESULT);
			}
			frame->stage = 2;
			fxNodeDispatchCode(((txWithNode*)self)->statement, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		default:
			coder->evalFlag = (txBoolean)frame->i0;
			coder->environmentLevel--;
			fxCoderAddByte(coder, 0, XS_CODE_WITHOUT);
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_RETURN:
		/* stock: fxReturnNodeCode — one expression dispatch; the rest is
		   native order around it. */
		switch (frame->stage) {
		case 0:
			frame->stage = 1;
			if (coder->programFlag)
				fxReportParserError(coder->parser, self->line, "invalid return");
			if (((txStatementNode*)self)->expression) {
				if (((self->flags & (mxStrictFlag | mxGeneratorFlag)) == mxStrictFlag) && (coder->returnTarget->original == NULL))
					((txStatementNode*)self)->expression->flags |= mxTailRecursionFlag;
				fxNodeDispatchCode(((txStatementNode*)self)->expression, coder);
				if (parser->nodeWalkStack != frame)
					return;
			}
			/* fall through */
		default:
			if (((txStatementNode*)self)->expression) {
				if ((self->flags & (mxAsyncFlag | mxGeneratorFlag)) == (mxAsyncFlag | mxGeneratorFlag)) {
					fxCoderAddByte(coder, 0, XS_CODE_AWAIT);
					fxCoderAddByte(coder, 0, XS_CODE_THROW_STATUS);
				}
				fxCoderAddByte(coder, -1, XS_CODE_SET_RESULT);
			}
			else if ((self->flags & (mxAsyncFlag | mxGeneratorFlag)) != (mxAsyncFlag | mxGeneratorFlag)) {
				fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
				fxCoderAddByte(coder, -1, XS_CODE_SET_RESULT);
			}
			fxCoderAdjustEnvironment(coder, coder->returnTarget);
			fxCoderAdjustScope(coder, coder->returnTarget);
			fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, coder->returnTarget);
			fxNodeWalkPopCode(parser);
		}
		return;
	/* xs_no_recursion (R7/B4b): for/for-in-of/switch/try/await/yield.
	   The long prologue/epilogue sequences are pure coder emissions kept
	   in their stock order; only child dispatches split stages. */
	case WC_FOR:
		/* stock: fxForNodeCode. i0=using flag; v0=continueTarget;
		   v1=nextTarget; v2=doneTarget. stages: 0 prologue+init,
		   1 cond, 2 body, 3 iteration, 4 tail. */
		switch (frame->stage) {
		case 0: {
				txForNode* node = (txForNode*)self;
				frame->v0 = coder->firstContinueTarget;
				coder->firstContinueTarget = ((txTargetCode*)frame->v0)->nextTarget;
				((txTargetCode*)frame->v0)->nextTarget = C_NULL;
				fxScopeCodingBlock(node->scope, coder);
				/* xs_no_recursion (R7/B5c): see running-clear note above. */
				{
					int running_nr = parser->nodeWalkRunning;
					parser->nodeWalkRunning = 0;
					fxScopeCodeDefineNodes(node->scope, coder);
					parser->nodeWalkRunning = running_nr;
				}
				if (node->scope->disposableNodeCount) {
					txUsingContext context;
					fxScopeCodeUsing(node->scope, coder, &context);
					frame->i0 = 0x1000 | context.exception;
					frame->i1 = context.selector;
					frame->v3 = context.catchTarget;
				}
				frame->v1 = fxCoderCreateTarget(coder);
				frame->v2 = fxCoderCreateTarget(coder);
				if (node->initialization) {
					frame->stage = 1;
					fxNodeDispatchCode(node->initialization, coder);
					if (parser->nodeWalkStack != frame)
						return;
				}
				goto wcfor_cond;
			}
		case 1:
		wcfor_cond:
			if (coder->programFlag) {
				fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
				fxCoderAddByte(coder, -1, XS_CODE_SET_RESULT);
			}
			fxScopeCodeRefresh(((txForNode*)self)->scope, coder);
			fxCoderAdd(coder, 0, (txTargetCode*)frame->v1);
			if (((txForNode*)self)->expression) {
				frame->stage = 2;
				fxNodeDispatchCode(((txForNode*)self)->expression, coder);
				if (parser->nodeWalkStack != frame)
					return;
				/* xs_no_recursion (R7/B5 fix): the dispatch completed
				   synchronously — emit the exit test here (case 2's
				   epilogue) instead of jumping past it, or the loop
				   condition is never tested at runtime (infinite loop,
				   stylis Q tokenizer). */
				goto wcfor_condtest;
			}
			goto wcfor_body;
		case 2:
		wcfor_condtest:
			fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_ELSE_1, (txTargetCode*)frame->v2);
		wcfor_body:
			((txTargetCode*)frame->v0)->environmentLevel = coder->environmentLevel;
			((txTargetCode*)frame->v0)->scopeLevel = coder->scopeLevel;
			((txTargetCode*)frame->v0)->stackLevel = coder->stackLevel;
			((txTargetCode*)frame->v0)->nextTarget = coder->firstContinueTarget;
			coder->firstContinueTarget = (txTargetCode*)frame->v0;
			frame->stage = 3;
			fxNodeDispatchCode(((txForNode*)self)->statement, coder);
			if (parser->nodeWalkStack != frame)
				return;
			goto wcfor_iter;
		case 3:
		wcfor_iter:
			fxCoderAdd(coder, 0, coder->firstContinueTarget);
			coder->firstContinueTarget = ((txTargetCode*)frame->v0)->nextTarget;
			((txTargetCode*)frame->v0)->nextTarget = C_NULL;
			if (((txForNode*)self)->iteration) {
				fxScopeCodeRefresh(((txForNode*)self)->scope, coder);
				((txForNode*)self)->iteration->flags |= mxExpressionNoValue;
				frame->stage = 4;
				fxNodeDispatchCode(((txForNode*)self)->iteration, coder);
				if (parser->nodeWalkStack != frame)
					return;
				/* xs_no_recursion (R7/B5 fix): sync completion — emit the
				   POP here (case 4's epilogue) instead of jumping past it,
				   or the iteration value is left on the stack. */
				goto wcfor_tailpop;
			}
			goto wcfor_tail;
		case 4:
		wcfor_tailpop:
			fxCoderAddByte(coder, -1, XS_CODE_POP);
		wcfor_tail:
			fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, (txTargetCode*)frame->v1);
			fxCoderAdd(coder, 0, (txTargetCode*)frame->v2);
			if (frame->i0 >= 0x1000) {
				txUsingContext context;
				context.exception = (frame->i0 & 0xFFF);
				context.selector = (txInteger)frame->i1;
				context.catchTarget = (txTargetCode*)frame->v3;
				fxScopeCodeUsed(((txForNode*)self)->scope, coder, &context);
			}
			fxScopeCoded(((txForNode*)self)->scope, coder);
			((txTargetCode*)frame->v0)->nextTarget = coder->firstContinueTarget;
			coder->firstContinueTarget = (txTargetCode*)frame->v0;
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_DELETE:
		/* stock: fxDeleteNodeCode — single Delete dispatch. */
		if (frame->stage == 0) {
			frame->stage = 1;
			fxNodeDispatchCodeDelete(((txDeleteNode*)self)->reference, coder);
			if (parser->nodeWalkStack != frame)
				return;
		}
		fxNodeWalkPopCode(parser);
		return;
	case WC_AWAIT:
		/* stock: fxAwaitNodeCode — expr dispatch, then status/return
		   epilogue. Flagless: stage is a pure phase cursor. */
		switch (frame->stage) {
		case 0:
			frame->stage = 1;
			fxNodeDispatchCode(((txStatementNode*)self)->expression, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		default: {
				txTargetCode* target = fxCoderCreateTarget(coder);
				fxCoderAddByte(coder, 0, XS_CODE_AWAIT);
				fxCoderAddBranch(coder, 1, XS_CODE_BRANCH_STATUS_1, target);
				fxCoderAddByte(coder, -1, XS_CODE_SET_RESULT);
				fxCoderAdjustEnvironment(coder, coder->returnTarget);
				fxCoderAdjustScope(coder, coder->returnTarget);
				fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, coder->returnTarget);
				fxCoderAdd(coder, 0, target);
				fxNodeWalkPopCode(parser);
			}
		}
		return;
	case WC_YIELD:
		/* stock: fxYieldNodeCode. i2 = async flag. */
		switch (frame->stage) {
		case 0: {
				txStatementNode* node = (txStatementNode*)self;
				frame->i2 = (self->flags & mxAsyncFlag) ? 1 : 0;
				if (!frame->i2) {
					fxCoderAddByte(coder, 1, XS_CODE_OBJECT);
					fxCoderAddByte(coder, 1, XS_CODE_DUB);
				}
				frame->stage = 1;
				fxNodeDispatchCode(node->expression, coder);
				if (parser->nodeWalkStack != frame)
					return;
			}
			/* fall through */
		default: {
				txTargetCode* target = fxCoderCreateTarget(coder);
				if (!frame->i2) {
					fxCoderAddSymbol(coder, -2, XS_CODE_NEW_PROPERTY, coder->parser->valueSymbol);
					fxCoderAddInteger(coder, 0, XS_CODE_INTEGER_1, 0);
					fxCoderAddByte(coder, 1, XS_CODE_DUB);
					fxCoderAddByte(coder, 1, XS_CODE_FALSE);
					fxCoderAddSymbol(coder, -2, XS_CODE_NEW_PROPERTY, coder->parser->doneSymbol);
					fxCoderAddInteger(coder, 0, XS_CODE_INTEGER_1, 0);
				}
				fxCoderAddByte(coder, 0, XS_CODE_YIELD);
				fxCoderAddBranch(coder, 1, XS_CODE_BRANCH_STATUS_1, target);
				if (frame->i2) {
					fxCoderAddByte(coder, 0, XS_CODE_AWAIT);
					fxCoderAddByte(coder, 0, XS_CODE_THROW_STATUS);
				}
				fxCoderAddByte(coder, -1, XS_CODE_SET_RESULT);
				fxCoderAdjustEnvironment(coder, coder->returnTarget);
				fxCoderAdjustScope(coder, coder->returnTarget);
				fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, coder->returnTarget);
				fxCoderAdd(coder, 0, target);
				fxNodeWalkPopCode(parser);
			}
		}
		return;
	case WC_SWITCH:
		/* stock: fxSwitchNodeCode. v0=item cursor; v1=breakTarget;
		   v2=defaultNode; i0=using flag; i1=break-push flag.
		   stages: 0 expr+prologue, 1 case-expr loop, 2 case-stmt loop,
		   3 resume case-stmt. */
		switch (frame->stage) {
		case 0: {
				txSwitchNode* node = (txSwitchNode*)self;
				frame->stage = 1;
				fxNodeDispatchCode(node->expression, coder);
				if (parser->nodeWalkStack != frame)
					return;
			}
			/* fall through */
		case 1: {
				txSwitchNode* node = (txSwitchNode*)self;
				fxScopeCodingBlock(node->scope, coder);
				if (node->scope->disposableNodeCount) {
					txUsingContext context;
					fxScopeCodeUsing(node->scope, coder, &context);
					frame->i0 = 0x1000 | context.exception;
					frame->i1 = context.selector;
					frame->v3 = context.catchTarget;
				}
				{
					txTargetCode* breakTarget = fxCoderCreateTarget(coder);
					breakTarget->label = fxNewParserChunkClear(coder->parser, sizeof(txLabelNode));
					breakTarget->nextTarget = coder->firstBreakTarget;
					coder->firstBreakTarget = breakTarget;
					frame->v1 = breakTarget;
				}
				if (coder->programFlag) {
					fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
					fxCoderAddByte(coder, -1, XS_CODE_SET_RESULT);
				}
				frame->v0 = node->items ? node->items->first : C_NULL;
				frame->v2 = C_NULL;
				while (frame->v0) {
					txCaseNode* caseNode = (txCaseNode*)frame->v0;
					caseNode->target = fxCoderCreateTarget(coder);
					if (caseNode->expression) {
						fxCoderAddByte(coder, 1, XS_CODE_DUB);
						frame->stage = 2;
						fxNodeDispatchCode(caseNode->expression, coder);
						if (parser->nodeWalkStack != frame)
							return;
						/* inline completion: emit its epilogue now */
						fxCoderAddByte(coder, -1, XS_CODE_STRICT_EQUAL);
						fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, caseNode->target);
					}
					else
						frame->v2 = caseNode;
					frame->v0 = caseNode->next;
				}
				if (frame->v2)
					fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, ((txCaseNode*)frame->v2)->target);
				else
					fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, (txTargetCode*)frame->v1);
				frame->v0 = node->items ? node->items->first : C_NULL;
				frame->stage = 3;
				goto wcswitch_stmts;
			}
		case 2:
			/* case expression completed: emit compare+branch, advance,
			   and resume the case-expr loop */
			{
				txCaseNode* caseNode = (txCaseNode*)frame->v0;
				fxCoderAddByte(coder, -1, XS_CODE_STRICT_EQUAL);
				fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, caseNode->target);
				frame->v0 = caseNode->next;
			}
			goto wcswitch_case_loop;
		case 3:
		wcswitch_resume:
			frame->v0 = ((txNode*)frame->v0)->next;
		wcswitch_case_loop:
			while (frame->v0) {
				txCaseNode* caseNode = (txCaseNode*)frame->v0;
				/* xs_no_recursion (R12 fix): cases visited for the first time
				   AFTER a case-expression suspension get their target here —
				   stock creates the target inside the single walk loop; the
				   machine's stage-1 pass only covers pre-suspension cases. */
				caseNode->target = fxCoderCreateTarget(coder);
				if (caseNode->expression) {
					fxCoderAddByte(coder, 1, XS_CODE_DUB);
					frame->stage = 2;
					fxNodeDispatchCode(caseNode->expression, coder);
					if (parser->nodeWalkStack != frame)
						return;
					fxCoderAddByte(coder, -1, XS_CODE_STRICT_EQUAL);
					fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, caseNode->target);
				}
				else
					frame->v2 = caseNode;
				frame->v0 = caseNode->next;
			}
			if (frame->v2)
				fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, ((txCaseNode*)frame->v2)->target);
			else
				fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, (txTargetCode*)frame->v1);
			frame->v0 = ((txSwitchNode*)self)->items ? ((txSwitchNode*)self)->items->first : C_NULL;
			frame->stage = 3;
			goto wcswitch_stmts;
		case 4:
		wcswitch_stmt_resume:
			/* statement child completed: advance past its case */
			frame->v0 = ((txNode*)frame->v0)->next;
		wcswitch_stmts:
			while (frame->v0) {
				txCaseNode* caseNode = (txCaseNode*)frame->v0;
				fxCoderAdd(coder, 0, caseNode->target);
				if (caseNode->statement) {
					frame->stage = 4;
					fxNodeDispatchCode(caseNode->statement, coder);
					if (parser->nodeWalkStack != frame)
						return;
					/* child completed inline: keep iterating */
				}
				frame->v0 = caseNode->next;
			}
		wcswitch_stmts_cont:
			fxCoderAdd(coder, 0, coder->firstBreakTarget);
			coder->firstBreakTarget = ((txTargetCode*)frame->v1)->nextTarget;
			if (frame->i0 >= 0x1000) {
				txUsingContext context;
				context.exception = (frame->i0 & 0xFFF);
				context.selector = (txInteger)frame->i1;
				context.catchTarget = (txTargetCode*)frame->v3;
				fxScopeCodeUsed(((txSwitchNode*)self)->scope, coder, &context);
			}
			fxScopeCoded(((txSwitchNode*)self)->scope, coder);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_FORINFOROF:
		/* stock: fxForInForOfNodeCode.
		   i0=iterator, i1=next, i2=done, i3=result, i4=exception,
		   i5=selector, i6=async, i7=resume phase.
		   v0=continueTarget, v1=nextTarget, v2=doneTarget,
		   v3=catchTarget, v4=normalTarget, v5=breakAlias,
		   v6=continueAlias, v7=returnAlias. stages as labeled. */
		switch (frame->stage) {
		case 0: {
				txForInForOfNode* node = (txForInForOfNode*)self;
				frame->i6 = (self->description->code == XS_CODE_FOR_AWAIT_OF) ? 1 : 0;
				frame->i0 = fxCoderUseTemporaryVariable(coder);
				frame->i1 = fxCoderUseTemporaryVariable(coder);
				frame->i2 = fxCoderUseTemporaryVariable(coder);
				frame->i3 = fxCoderUseTemporaryVariable(coder);
				frame->i4 = fxCoderUseTemporaryVariable(coder);
				frame->i5 = fxCoderUseTemporaryVariable(coder);
				frame->v0 = coder->firstContinueTarget;
				coder->firstContinueTarget = ((txTargetCode*)frame->v0)->nextTarget;
				((txTargetCode*)frame->v0)->nextTarget = C_NULL;
				fxScopeCodingBlock(node->scope, coder);
				/* xs_no_recursion (R7/B5c): see running-clear note above. */
				{
					int running_nr = parser->nodeWalkRunning;
					parser->nodeWalkRunning = 0;
					fxScopeCodeDefineNodes(node->scope, coder);
					parser->nodeWalkRunning = running_nr;
				}
				if (coder->programFlag) {
					fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
					fxCoderAddByte(coder, -1, XS_CODE_SET_RESULT);
				}
				frame->stage = 1;
				fxNodeDispatchCode(node->expression, coder);
				if (parser->nodeWalkStack != frame)
					return;
			}
			/* fall through */
		case 1: {
				txForInForOfNode* node = (txForInForOfNode*)self;
				fxCoderAddByte(coder, 0, self->description->code);
				fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->i0);
				fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->nextSymbol);
				fxCoderAddIndex(coder, 0, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i1);
				frame->v5 = fxCoderAliasTargets(coder, coder->firstBreakTarget);
				coder->firstBreakTarget = (txTargetCode*)frame->v5;
				frame->v6 = fxCoderAliasTargets(coder, coder->firstContinueTarget);
				coder->firstContinueTarget = (txTargetCode*)frame->v6;
				frame->v7 = fxCoderAliasTargets(coder, coder->returnTarget);
				coder->returnTarget = (txTargetCode*)frame->v7;
				frame->v3 = fxCoderCreateTarget(coder);
				frame->v4 = fxCoderCreateTarget(coder);
				fxCoderAddBranch(coder, 0, XS_CODE_CATCH_1, (txTargetCode*)frame->v3);
				/* LOOP */
				frame->v1 = fxCoderCreateTarget(coder);
				fxCoderAdd(coder, 0, (txTargetCode*)frame->v1);
				fxCoderAddByte(coder, 1, XS_CODE_TRUE);
				fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i2);
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i0);
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i1);
				fxCoderAddByte(coder, 1, XS_CODE_CALL);
				fxCoderAddInteger(coder, -2, XS_CODE_RUN_1, 0);
				if (frame->i6) {
					fxCoderAddByte(coder, 0, XS_CODE_AWAIT);
					fxCoderAddByte(coder, 0, XS_CODE_THROW_STATUS);
				}
				fxCoderAddByte(coder, 0, XS_CODE_CHECK_INSTANCE);
				fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->i3);
				fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->doneSymbol);
				fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->i2);
				fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, (txTargetCode*)frame->v4);
				fxScopeCodeReset(node->scope, coder);
				/* xs_no_recursion (R7/B5c): bump stage BEFORE the dispatch — a
				   parked Reference/Binding child must not re-enter stage 1. */
				frame->stage = 2;
				fxNodeDispatchCodeReference(node->reference, coder, 0);
				if (parser->nodeWalkStack != frame)
					return;
			}
			/* fall through */
		case 2: {
				txForInForOfNode* node = (txForInForOfNode*)self;
				fxCoderAddByte(coder, 1, XS_CODE_TRUE);
				fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i2);
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i3);
				fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->valueSymbol);
				fxCoderAddByte(coder, 1, XS_CODE_FALSE);
				fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i2);
				frame->stage = 3;
				fxNodeDispatchCodeAssign(node->reference, coder, 0);
				if (parser->nodeWalkStack != frame)
					return;
			}
			/* fall through */
		case 3:
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			((txTargetCode*)frame->v0)->environmentLevel = coder->environmentLevel;
			((txTargetCode*)frame->v0)->scopeLevel = coder->scopeLevel;
			((txTargetCode*)frame->v0)->stackLevel = coder->stackLevel;
			((txTargetCode*)frame->v0)->nextTarget = coder->firstContinueTarget;
			coder->firstContinueTarget = (txTargetCode*)frame->v0;
			frame->stage = 4;
			fxNodeDispatchCode(((txForInForOfNode*)self)->statement, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 4:
		wcfo_body_done:
			/* body completed: close the loop, then the PRE FINALLY block */
			{
				txForInForOfNode* node = (txForInForOfNode*)self;
				fxCoderAdd(coder, 0, coder->firstContinueTarget);
				coder->firstContinueTarget = ((txTargetCode*)frame->v0)->nextTarget;
				((txTargetCode*)frame->v0)->nextTarget = C_NULL;
				fxScopeCodeUsedReverse(node->scope, coder, node->scope->firstDeclareNode, (txInteger)frame->i4, (txInteger)frame->i5);
				fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, (txTargetCode*)frame->v1);
				/* PRE FINALLY */
				{
					txTargetCode* uncatchTarget = fxCoderCreateTarget(coder);
					txTargetCode* finallyTarget = fxCoderCreateTarget(coder);
					txInteger selection;
					fxCoderAdd(coder, 0, (txTargetCode*)frame->v3);
					fxCoderAddByte(coder, 1, XS_CODE_EXCEPTION);
					fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i4);
					fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, 0);
					fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i5);
					fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, finallyTarget);
					selection = 1;
					coder->firstBreakTarget = fxCoderFinalizeTargets(coder, (txTargetCode*)frame->v5, (txInteger)frame->i5, &selection, uncatchTarget);
					coder->firstContinueTarget = fxCoderFinalizeTargets(coder, (txTargetCode*)frame->v6, (txInteger)frame->i5, &selection, uncatchTarget);
					coder->returnTarget = fxCoderFinalizeTargets(coder, (txTargetCode*)frame->v7, (txInteger)frame->i5, &selection, finallyTarget);
					fxCoderAdd(coder, 0, (txTargetCode*)frame->v4);
					fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, selection);
					fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i5);
					fxCoderAdd(coder, 0, uncatchTarget);
					fxCoderAddByte(coder, 0, XS_CODE_UNCATCH);
					fxCoderAdd(coder, 0, finallyTarget);
				}
				/* FINALLY: iterator.return protocol */
				{
					txTargetCode* catchTarget = fxCoderCreateTarget(coder);
					txTargetCode* normalTarget = fxCoderCreateTarget(coder);
					txTargetCode* doneTarget = fxCoderCreateTarget(coder);
					txTargetCode* returnTarget = fxCoderCreateTarget(coder);
					txTargetCode* nextTarget = fxCoderCreateTarget(coder);
					txTargetCode* elseTarget = fxCoderCreateTarget(coder);
					txInteger selection;
					frame->v2 = normalTarget;	/* reuse for POST FINALLY close */
					fxCoderAddBranch(coder, 0, XS_CODE_CATCH_1, catchTarget);
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i2);
					fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, doneTarget);
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i0);
					fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->returnSymbol);
					fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_CHAIN_1, returnTarget);
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i0);
					fxCoderAddByte(coder, 0, XS_CODE_SWAP);
					fxCoderAddByte(coder, 1, XS_CODE_CALL);
					fxCoderAddInteger(coder, -2, XS_CODE_RUN_1, 0);
					if (frame->i6) {
						fxCoderAddByte(coder, 0, XS_CODE_AWAIT);
						fxCoderAddByte(coder, 0, XS_CODE_THROW_STATUS);
					}
					fxCoderAddByte(coder, 0, XS_CODE_CHECK_INSTANCE);
					fxCoderAdd(coder, 0, returnTarget);
					fxCoderAddByte(coder, -1, XS_CODE_POP);
					fxCoderAdd(coder, 0, doneTarget);
					fxCoderAddByte(coder, 0, XS_CODE_UNCATCH);
					fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, normalTarget);
					fxCoderAdd(coder, 0, catchTarget);
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i5);
					fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_ELSE_1, normalTarget);
					fxCoderAddByte(coder, 1, XS_CODE_EXCEPTION);
					fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i4);
					fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, 0);
					fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i5);
					fxCoderAdd(coder, 0, normalTarget);
					fxScopeCodeUsedReverse(node->scope, coder, node->scope->firstDeclareNode, (txInteger)frame->i4, (txInteger)frame->i5);
					/* POST FINALLY */
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i5);
					fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, elseTarget);
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i4);
					fxCoderAddByte(coder, -1, XS_CODE_THROW);
					fxCoderAdd(coder, 0, elseTarget);
					selection = 1;
					fxCoderJumpTargets(coder, coder->firstBreakTarget, (txInteger)frame->i5, &selection);
					fxCoderJumpTargets(coder, coder->firstContinueTarget, (txInteger)frame->i5, &selection);
					fxCoderJumpTargets(coder, coder->returnTarget, (txInteger)frame->i5, &selection);
					fxScopeCoded(node->scope, coder);
					((txTargetCode*)frame->v0)->nextTarget = coder->firstContinueTarget;
					coder->firstContinueTarget = (txTargetCode*)frame->v0;
					fxCoderUnuseTemporaryVariables(coder, 6);
				}
				fxNodeWalkPopCode(parser);
			}
		}
		return;
	case WC_CATCH:
		/* stock: fxCatchNodeCode — parameter vs no-parameter shapes.
		   R7/B5c rewrite: every dispatch bumps the stage FIRST (a parked
		   child re-enters at the bumped stage; the old machine left stage 0
		   set across the Reference dispatch → infinite re-dispatch, and
		   jumped to the statement stage across the Assign dispatch → body
		   skipped). Note stock's fxScopeCodeUsingStatement is a whole-
		   statement encoder and must NOT be called by the machine; the
		   using protocol is the fxScopeCodeUsing here + fxScopeCodeUsed in
		   the epilogue. i4 = exception|0x1000 flag, i5 = selector,
		   v4 = catchTarget. stages: 0 prologue, 1 after Reference,
		   2 after Assign, 3 after statement (no-param), 4 after statement
		   (param). */
		switch (frame->stage) {
		case 0: {
				txCatchNode* node = (txCatchNode*)self;
				if (node->parameter) {
					fxScopeCodingBlock(node->scope, coder);
					frame->stage = 1;
					fxNodeDispatchCodeReference(node->parameter, coder, 0);
					if (parser->nodeWalkStack != frame)
						return;
					goto wccatch_ref_done;
				}
				else {
					fxScopeCodingBlock(node->statementScope, coder);
					/* xs_no_recursion (R7/B5c): see running-clear note at WC_BLOCK. */
					{
						int running_nr = parser->nodeWalkRunning;
						parser->nodeWalkRunning = 0;
						fxScopeCodeDefineNodes(node->statementScope, coder);
						parser->nodeWalkRunning = running_nr;
					}
					/* xs_no_recursion (R7/B5c): stock's fxScopeCodeUsingStatement
					   is a whole-statement ENCODER (its else branch dispatches
					   the statement), so the machine must NOT call it — its
					   using-half is the i4/v4 context + fxScopeCodeUsed in the
					   epilogue below, its dispatch-half is this stage dispatch. */
					frame->stage = 3;
					fxNodeDispatchCode(node->statement, coder);
					if (parser->nodeWalkStack != frame)
						return;
					goto wccatch_noparam_done;
				}
			}
		case 1:
		wccatch_ref_done:
			fxCoderAddByte(coder, 1, XS_CODE_EXCEPTION);
			frame->stage = 2;
			fxNodeDispatchCodeAssign(((txCatchNode*)self)->parameter, coder, 0);
			if (parser->nodeWalkStack != frame)
				return;
			goto wccatch_assign_done;
		case 2:
		wccatch_assign_done:
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			fxScopeCodingBlock(((txCatchNode*)self)->statementScope, coder);
			/* xs_no_recursion (R7/B5c): see running-clear note at WC_BLOCK. */
			{
				int running_nr = parser->nodeWalkRunning;
				parser->nodeWalkRunning = 0;
				fxScopeCodeDefineNodes(((txCatchNode*)self)->statementScope, coder);
				parser->nodeWalkRunning = running_nr;
			}
			if (((txCatchNode*)self)->statementScope->disposableNodeCount) {
				txUsingContext context;
				fxScopeCodeUsing(((txCatchNode*)self)->statementScope, coder, &context);
				frame->i4 = 0x1000 | context.exception;
				frame->i5 = context.selector;
				frame->v4 = context.catchTarget;
			}
			frame->stage = 4;
			fxNodeDispatchCode(((txCatchNode*)self)->statement, coder);
			if (parser->nodeWalkStack != frame)
				return;
			goto wccatch_stmt_done;
		case 3:
		wccatch_noparam_done:
			if (frame->i4 >= 0x1000) {
				txUsingContext context;
				context.exception = (frame->i4 & 0xFFF);
				context.selector = (txInteger)frame->i5;
				context.catchTarget = (txTargetCode*)frame->v4;
				fxScopeCodeUsed(((txCatchNode*)self)->statementScope, coder, &context);
			}
			fxScopeCoded(((txCatchNode*)self)->statementScope, coder);
			fxNodeWalkPopCode(parser);
			return;
		case 4:
		wccatch_stmt_done:
			if (frame->i4 >= 0x1000) {
				txUsingContext context;
				context.exception = (frame->i4 & 0xFFF);
				context.selector = (txInteger)frame->i5;
				context.catchTarget = (txTargetCode*)frame->v4;
				fxScopeCodeUsed(((txCatchNode*)self)->statementScope, coder, &context);
			}
			fxScopeCoded(((txCatchNode*)self)->statementScope, coder);
			fxScopeCoded(((txCatchNode*)self)->scope, coder);
			fxNodeWalkPopCode(parser);
			return;
		}
		return;
	case WC_TRY:
		/* stock: fxTryNodeCode. i0=exception, i1=selector, i2=result,
		   i3=using flag; v0=catchTarget, v1=normalTarget, v2=finallyTarget,
		   v3=catchTarget2, v4=tryAlias, v5=contAlias. stages:
		   0 prologue, 1 try, 2 catch, 3 mid-epilogue, 4 finally,
		   5 post-tail. */
		switch (frame->stage) {
		case 0: {
				txTryNode* node = (txTryNode*)self;
				frame->i0 = fxCoderUseTemporaryVariable(coder);
				frame->i1 = fxCoderUseTemporaryVariable(coder);
				frame->i2 = fxCoderUseTemporaryVariable(coder);
				frame->v4 = fxCoderAliasTargets(coder, coder->firstBreakTarget);
				coder->firstBreakTarget = (txTargetCode*)frame->v4;
				frame->v5 = fxCoderAliasTargets(coder, coder->firstContinueTarget);
				coder->firstContinueTarget = (txTargetCode*)frame->v5;
				{
					txTargetCode* alias = fxCoderAliasTargets(coder, coder->returnTarget);
					frame->v6 = alias;
					coder->returnTarget = alias;
				}
				frame->v0 = fxCoderCreateTarget(coder);
				frame->v1 = fxCoderCreateTarget(coder);
				frame->v2 = fxCoderCreateTarget(coder);
				fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, 0);
				fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->i1);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
				fxCoderAddBranch(coder, 0, XS_CODE_CATCH_1, (txTargetCode*)frame->v0);
				if (coder->programFlag) {
					fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
					fxCoderAddByte(coder, -1, XS_CODE_SET_RESULT);
				}
				frame->stage = 1;
				fxNodeDispatchCode(node->tryBlock, coder);
				if (parser->nodeWalkStack != frame)
					return;
			}
			/* fall through */
		case 1: {
				txTryNode* node = (txTryNode*)self;
				fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, (txTargetCode*)frame->v1);
				if (node->catchBlock) {
					fxCoderAddByte(coder, 0, XS_CODE_UNCATCH);
					fxCoderAdd(coder, 0, (txTargetCode*)frame->v0);
					frame->v0 = fxCoderCreateTarget(coder);
					fxCoderAddBranch(coder, 0, XS_CODE_CATCH_1, (txTargetCode*)frame->v0);
					if (coder->programFlag) {
						fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
						fxCoderAddByte(coder, -1, XS_CODE_SET_RESULT);
					}
					frame->stage = 2;
					fxNodeDispatchCode(node->catchBlock, coder);
					if (parser->nodeWalkStack != frame)
						return;
					/* xs_no_recursion (R7/B5 fix): sync completion — emit
					   the branch to normalTarget (case 2's epilogue) instead
					   of jumping past it, or control falls into the exception
					   path after the catch block. */
					goto wctry_catchdone;
				}
				goto wctry_mid;
			}
		case 2:
		wctry_catchdone:
			fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, (txTargetCode*)frame->v1);
		wctry_mid:
			/* finalize break/continue/return onto finally */
			{
				txInteger selection = 1;
				coder->firstBreakTarget = fxCoderFinalizeTargets(coder, (txTargetCode*)frame->v4, (txInteger)frame->i1, &selection, (txTargetCode*)frame->v2);
				coder->firstContinueTarget = fxCoderFinalizeTargets(coder, (txTargetCode*)frame->v5, (txInteger)frame->i1, &selection, (txTargetCode*)frame->v2);
				coder->returnTarget = fxCoderFinalizeTargets(coder, (txTargetCode*)frame->v6, (txInteger)frame->i1, &selection, (txTargetCode*)frame->v2);
				fxCoderAdd(coder, 0, (txTargetCode*)frame->v1);
				fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, selection);
				fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->i1);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
				fxCoderAdd(coder, 0, (txTargetCode*)frame->v2);
				fxCoderAddByte(coder, 0, XS_CODE_UNCATCH);
				fxCoderAdd(coder, 0, (txTargetCode*)frame->v0);
				fxCoderAddByte(coder, 1, XS_CODE_EXCEPTION);
				fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->i0);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
			}
			if (((txTryNode*)self)->finallyBlock) {
				if (coder->programFlag) {
					fxCoderAddByte(coder, 1, XS_CODE_GET_RESULT);
					fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i2);
					fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
					fxCoderAddByte(coder, -1, XS_CODE_SET_RESULT);
				}
				frame->stage = 3;
				fxNodeDispatchCode(((txTryNode*)self)->finallyBlock, coder);
				if (parser->nodeWalkStack != frame)
					return;
				/* xs_no_recursion (R7/B5 fix): sync completion — restore the
				   stashed result (case 3's epilogue) instead of jumping past
				   it. */
				goto wctry_finallydone;
			}
			goto wctry_tail;
		case 3:
		wctry_finallydone:
			if (coder->programFlag) {
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i2);
				fxCoderAddByte(coder, -1, XS_CODE_SET_RESULT);
			}
		wctry_tail:
			{
				txInteger selection = 1;
				frame->v0 = fxCoderCreateTarget(coder);
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i1);
				fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, (txTargetCode*)frame->v0);
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i0);
				fxCoderAddByte(coder, -1, XS_CODE_THROW);
				fxCoderAdd(coder, 0, (txTargetCode*)frame->v0);
				fxCoderJumpTargets(coder, coder->firstBreakTarget, (txInteger)frame->i1, &selection);
				fxCoderJumpTargets(coder, coder->firstContinueTarget, (txInteger)frame->i1, &selection);
				fxCoderJumpTargets(coder, coder->returnTarget, (txInteger)frame->i1, &selection);
				fxCoderUnuseTemporaryVariables(coder, 3);
			}
			fxNodeWalkPopCode(parser);
		}
		return;
	/* R7/B5a: stock fxParamsNodeCode. Non-spread: RUN_1(int c) after all
	   items; spread: counter temp + per-item increments + RUN/EVAL. */
	case WC_PARAMS:
		switch (frame->stage) {
		case 0: {
				txParamsNode* node = (txParamsNode*)self;
				if (node->flags & mxSpreadFlag) {
					frame->i0 = fxCoderUseTemporaryVariable(coder);
					fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, 0);
					fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->i0);
					fxCoderAddByte(coder, -1, XS_CODE_POP);
					frame->v0 = node->items ? node->items->first : NULL;
					frame->i1 = 0; /* c */
				}
				else {
					frame->v0 = node->items ? node->items->first : NULL;
					frame->i1 = 0;
				}
				frame->stage = 1;
				/* fall through to loop head */
			}
			/* fall through */
		case 1:
			/* advance-loop: dispatch one item at a time; advance cursor on
			   revisit so a parked frame doesn't re-run the same item */
			while (frame->v0) {
				txNode* item = (txNode*)frame->v0;
				frame->v0 = item->next;
				if (((txParamsNode*)self)->flags & mxSpreadFlag) {
				if (item->description->token == XS_TOKEN_SPREAD) {
					/* xs_no_recursion (R7/B5c): plain-code helper run from a
					   machine stage: clear running so its dispatch completes
					   inline — a parked spread frame would resume after this
					   frame and skip the post-call counter emissions. */
					int running_nr = parser->nodeWalkRunning;
					parser->nodeWalkRunning = 0;
					fxSpreadNodeCode(item, coder, (txInteger)frame->i0);
					parser->nodeWalkRunning = running_nr;
				}
					else {
						((txParamsNode*)self)->flags &= ~0;
						frame->i1 = frame->i1 + 1;
						fxNodeDispatchCode(item, coder);
						if (parser->nodeWalkStack != frame)
							return;
						fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i0);
						fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, 1);
						fxCoderAddByte(coder, -1, XS_CODE_ADD);
						fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->i0);
						fxCoderAddByte(coder, -1, XS_CODE_POP);
					}
				}
				else {
					frame->i1 = frame->i1 + 1;
					fxNodeDispatchCode(item, coder);
					if (parser->nodeWalkStack != frame)
						return;
				}
			}
			{
				txParamsNode* node = (txParamsNode*)self;
				if (node->flags & mxSpreadFlag) {
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i0);
					if (node->flags & mxEvalParametersFlag)
						fxCoderAddByte(coder, -3 - (int)frame->i1, (node->flags & mxTailRecursionFlag) ? XS_CODE_EVAL_TAIL : XS_CODE_EVAL);
					else
						fxCoderAddByte(coder, -3 - (int)frame->i1, (node->flags & mxTailRecursionFlag) ? XS_CODE_RUN_TAIL : XS_CODE_RUN);
					fxCoderUnuseTemporaryVariables(coder, 1);
				}
				else {
					if (node->flags & mxEvalParametersFlag) {
						fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, (int)frame->i1);
						fxCoderAddByte(coder, -3 - (int)frame->i1, (node->flags & mxTailRecursionFlag) ? XS_CODE_EVAL_TAIL : XS_CODE_EVAL);
					}
					else
						fxCoderAddInteger(coder, -2 - (int)frame->i1, (node->flags & mxTailRecursionFlag) ? XS_CODE_RUN_TAIL_1 : XS_CODE_RUN_1, (int)frame->i1);
				}
			}
			fxNodeWalkPopCode(parser);
			return;
		}
		return;
	/* R7/B5a: stock fxObjectNodeCode. v0=object temp, v1=item cursor,
	   i0=__proto__ flag; prologue scans for __proto__ first. */
	case WC_OBJECT: {
		txObjectNode* node = (txObjectNode*)self;
		/* epilogue for the item the cursor last advanced past */
		#define WC_OBJECT_ITEM_TAIL(item) \
			do { \
				txNode* item_ = (item); \
				txFlag flag_ = 0; \
				txNode* value_ = (item_->description->token == XS_TOKEN_PROPERTY) \
					? ((txPropertyNode*)item_)->value \
					: (txNode*)((txPropertyAtNode*)item_)->value; \
				if (item_->description->token == XS_TOKEN_PROPERTY) \
					fxCoderAddSymbol(coder, -2, XS_CODE_NEW_PROPERTY, ((txPropertyNode*)item_)->symbol); \
				else \
					fxCoderAddByte(coder, -3, XS_CODE_NEW_PROPERTY_AT); \
				if (item_->flags & mxMethodFlag) \
					flag_ |= XS_NAME_FLAG | XS_METHOD_FLAG; \
				else if (item_->flags & mxGetterFlag) \
					flag_ |= XS_NAME_FLAG | XS_METHOD_FLAG | XS_GETTER_FLAG; \
				else if (item_->flags & mxSetterFlag) \
					flag_ |= XS_NAME_FLAG | XS_METHOD_FLAG | XS_SETTER_FLAG; \
				else if (fxNodeCodeName(value_)) \
					flag_ |= XS_NAME_FLAG; \
				fxCoderAddInteger(coder, 0, XS_CODE_INTEGER_1, flag_); \
			} while (0)
		switch (frame->stage) {
		case 0: {
				txNode* item;
				frame->i0 = fxCoderUseTemporaryVariable(coder);
				frame->i1 = 0; /* __proto__ seen flag */
				if (node->items) {
					item = node->items->first;
					while (item) {
						if ((item->description->token == XS_TOKEN_PROPERTY) &&
							!(item->flags & mxShorthandFlag) &&
							(((txPropertyNode*)item)->symbol == coder->parser->__proto__Symbol)) {
							if (frame->i1)
								fxReportParserError(coder->parser, item->line, "invalid __proto__");
							frame->i1 = 1;
							fxNodeDispatchCode(((txPropertyNode*)item)->value, coder);
							if (parser->nodeWalkStack != frame)
								return;
							fxCoderAddByte(coder, 0, XS_CODE_INSTANTIATE);
						}
						item = item->next;
					}
				}
				if (!frame->i1)
					fxCoderAddByte(coder, 1, XS_CODE_OBJECT);
				fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->i0);
				frame->v1 = node->items ? node->items->first : NULL;
				frame->stage = 1;
			}
			/* fall through */
		case 1:
			goto wcobject_loop;
		case 2: /* resume: property value child parked */
			WC_OBJECT_ITEM_TAIL((txNode*)frame->v2);
			frame->v1 = ((txNode*)frame->v2)->next;
			frame->stage = 1;
			goto wcobject_loop;
		case 3: /* resume: property-at 'at' child parked */
			{
				txNode* item = (txNode*)frame->v2;
				fxCoderAddByte(coder, 0, XS_CODE_AT);
				fxNodeDispatchCode(((txPropertyAtNode*)item)->value, coder);
				if (parser->nodeWalkStack != frame) {
					frame->stage = 4;
					return;
				}
				WC_OBJECT_ITEM_TAIL(item);
				frame->v1 = item->next;
				frame->stage = 1;
				goto wcobject_loop;
			}
		case 4: /* resume: property-at value child parked */
			WC_OBJECT_ITEM_TAIL((txNode*)frame->v2);
			frame->v1 = ((txNode*)frame->v2)->next;
			frame->stage = 1;
			goto wcobject_loop;
		}
		return;
	wcobject_loop:
		/* shared item loop: entered from stage 1 and resume stages */
		while (frame->v1) {
			txNode* item = (txNode*)frame->v1;
			txNode* value;
			if (item->description->token == XS_TOKEN_SPREAD) {
				frame->v1 = item->next;
				fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
				fxCoderAddByte(coder, 1, XS_CODE_COPY_OBJECT);
				fxCoderAddByte(coder, 1, XS_CODE_CALL);
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i0);
				fxNodeDispatchCode(((txSpreadNode*)item)->expression, coder);
				if (parser->nodeWalkStack != frame)
					return;
				fxCoderAddInteger(coder, -4, XS_CODE_RUN_1, 2);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
				continue;
			}
			if (item->description->token == XS_TOKEN_PROPERTY) {
				if (!(item->flags & mxShorthandFlag) && (((txPropertyNode*)item)->symbol == coder->parser->__proto__Symbol)) {
					frame->v1 = item->next;
					continue;
				}
				value = ((txPropertyNode*)item)->value;
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i0);
				fxNodeDispatchCode(value, coder);
				if (parser->nodeWalkStack != frame) {
					frame->v2 = item; /* parked mid-item: tail runs on resume */
					frame->stage = 2;
					return;
				}
				WC_OBJECT_ITEM_TAIL(item);
			}
			else {
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i0);
				fxNodeDispatchCode(((txPropertyAtNode*)item)->at, coder);
				if (parser->nodeWalkStack != frame) {
					frame->v2 = item; /* parked: resume re-runs AT + value + tail */
					frame->stage = 3;
					return;
				}
				fxCoderAddByte(coder, 0, XS_CODE_AT);
				value = ((txPropertyAtNode*)item)->value;
				fxNodeDispatchCode(value, coder);
				if (parser->nodeWalkStack != frame) {
					frame->v2 = item;
					frame->stage = 4;
					return;
				}
				WC_OBJECT_ITEM_TAIL(item);
			}
			frame->v1 = item->next;
		}
		fxCoderUnuseTemporaryVariables(coder, 1);
		fxNodeWalkPopCode(parser);
		return;
	#undef WC_OBJECT_ITEM_TAIL
	}
	/* stock: fxCallNodeCode — This(reference, 0); CALL; params. */
	case WC_CALL:
		switch (frame->stage) {
		case 0:
			frame->stage = 1;
			fxNodeDispatchCodeThis(((txCallNewNode*)self)->reference, coder, 0);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 1:
			fxCoderAddByte(coder, 1, XS_CODE_CALL);
			((txCallNewNode*)self)->params->flags |= self->flags & mxTailRecursionFlag;
			frame->stage = 2;
			fxNodeDispatchCode(((txCallNewNode*)self)->params, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 2:
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_NEW:
		/* stock: fxNewNodeCode — reference; NEW; params. */
		switch (frame->stage) {
		case 0:
			frame->stage = 1;
			fxNodeDispatchCode(((txCallNewNode*)self)->reference, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 1:
			fxCoderAddByte(coder, 2, XS_CODE_NEW);
			frame->stage = 2;
			fxNodeDispatchCode(((txCallNewNode*)self)->params, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 2:
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_CHAIN:
		/* stock: fxChainNodeCode — save/swap coder->chainTarget, dispatch
		   right, close target, restore. */
		if (frame->stage == 0) {
			frame->stage = 1;
			frame->v0 = coder->chainTarget;
			coder->chainTarget = fxCoderCreateTarget(coder);
			fxNodeDispatchCode(((txUnaryExpressionNode*)self)->right, coder);
			if (parser->nodeWalkStack != frame)
				return;
			fxCoderAdd(coder, 0, coder->chainTarget);
			coder->chainTarget = (txTargetCode*)frame->v0;
			fxNodeWalkPopCode(parser);
			return;
		}
		/* revisit: child completed */
		fxCoderAdd(coder, 0, coder->chainTarget);
		coder->chainTarget = (txTargetCode*)frame->v0;
		fxNodeWalkPopCode(parser);
		return;
	case WC_OPTION:
		/* stock: fxOptionNodeCode — tail flag; dispatch right;
		   BRANCH_CHAIN_1 to coder->chainTarget. */
		if (frame->stage == 0) {
			frame->stage = 1;
			((txUnaryExpressionNode*)self)->right->flags |= (self->flags & mxTailRecursionFlag);
			fxNodeDispatchCode(((txUnaryExpressionNode*)self)->right, coder);
			if (parser->nodeWalkStack != frame)
				return;
			fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_CHAIN_1, coder->chainTarget);
			fxNodeWalkPopCode(parser);
			return;
		}
		fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_CHAIN_1, coder->chainTarget);
		fxNodeWalkPopCode(parser);
		return;
	case WC_MEMBER_AT:
		/* stock: fxMemberAtNodeCode — reference; at; SUPER_AT/AT;
		   GET_SUPER_AT/GET_PROPERTY_AT. */
		switch (frame->stage) {
		case 0:
			frame->stage = 1;
			fxNodeDispatchCode(((txMemberAtNode*)self)->reference, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 1:
			frame->stage = 2;
			fxNodeDispatchCode(((txMemberAtNode*)self)->at, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 2:
			fxCoderAddByte(coder, 0, (((txMemberAtNode*)self)->reference->flags & mxSuperFlag) ? XS_CODE_SUPER_AT : XS_CODE_AT);
			fxCoderAddByte(coder, -1, (((txMemberAtNode*)self)->reference->flags & mxSuperFlag) ? XS_CODE_GET_SUPER_AT : XS_CODE_GET_PROPERTY_AT);
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_PRIVATE_MEMBER:
		/* stock: fxPrivateMemberNodeCode — reference; GET_PRIVATE_1. */
		if (frame->stage == 0) {
			frame->stage = 1;
			fxNodeDispatchCode(((txPrivateMemberNode*)self)->reference, coder);
			if (parser->nodeWalkStack != frame)
				return;
			fxCoderAddIndex(coder, 0, XS_CODE_GET_PRIVATE_1, ((txPrivateMemberNode*)self)->declaration->index);
			fxNodeWalkPopCode(parser);
			return;
		}
		fxCoderAddIndex(coder, 0, XS_CODE_GET_PRIVATE_1, ((txPrivateMemberNode*)self)->declaration->index);
		fxNodeWalkPopCode(parser);
		return;
	case WC_PRIVATE_MEMBER_DELETE:
		/* stock: fxPrivateMemberNodeCodeDelete — reference; parser error. */
		if (frame->stage == 0) {
			frame->stage = 1;
			fxNodeDispatchCode(((txPrivateMemberNode*)self)->reference, coder);
			if (parser->nodeWalkStack != frame)
				return;
			fxReportParserError(coder->parser, self->line, "delete private property");
			fxNodeWalkPopCode(parser);
			return;
		}
		fxReportParserError(coder->parser, self->line, "delete private property");
		fxNodeWalkPopCode(parser);
		return;
	case WC_PRIVATE_MEMBER_ASSIGN:
		/* stock: fxPrivateMemberNodeCodeAssign — pure epilogue. */
		fxCoderAddIndex(coder, -1, XS_CODE_SET_PRIVATE_1, ((txPrivateMemberNode*)self)->declaration->index);
		fxNodeWalkPopCode(parser);
		return;
	case WC_PRIVATE_MEMBER_REFERENCE:
		/* stock: fxPrivateMemberNodeCodeReference — single dispatch; the
		   flag is unused. Reference dispatch passes flag via stage, so
		   phase tracking lives in i0. */
		if (frame->i0 == 0) {
			frame->i0 = 1;
			fxNodeDispatchCode(((txPrivateMemberNode*)self)->reference, coder);
			if (parser->nodeWalkStack != frame)
				return;
		}
		fxNodeWalkPopCode(parser);
		return;
	case WC_CHAIN_THIS:
		/* stock: fxChainNodeCodeThis. Incoming flag rides frame->stage
		   (This dispatch): saved to i2 at phase 0; phase tracking lives
		   in i0; the inner result lands in parser->codeThisResult, i1
		   keeps it across the close/restore. */
		switch (frame->i0) {
		case 0:
			frame->i0 = 1;
			frame->i2 = frame->stage;
			frame->v0 = coder->chainTarget;
			coder->chainTarget = fxCoderCreateTarget(coder);
			fxNodeDispatchCodeThis(((txUnaryExpressionNode*)self)->right, coder, (txFlag)frame->i2);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		default:
			frame->i1 = parser->codeThisResult;
			fxCoderAdd(coder, 0, coder->chainTarget);
			coder->chainTarget = (txTargetCode*)frame->v0;
			parser->codeThisResult = (txInteger)frame->i1;
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_OPTION_THIS:
		/* stock: fxOptionNodeCodeThis — two targets up front, This(right),
		   then the BRANCH_CHAIN/BRANCH/SWAP/POP/BRANCH sequence. Flag
		   rides frame->stage (This dispatch); phase tracking in i0. */
		switch (frame->i0) {
		case 0:
			frame->i0 = 1;
			frame->i2 = frame->stage;
			frame->v0 = fxCoderCreateTarget(coder);		/* swapTarget */
			frame->v1 = fxCoderCreateTarget(coder);		/* skipTarget */
			((txUnaryExpressionNode*)self)->right->flags |= (self->flags & mxTailRecursionFlag);
			fxNodeDispatchCodeThis(((txUnaryExpressionNode*)self)->right, coder, (txFlag)frame->i2);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		default:
			frame->i1 = parser->codeThisResult;
			fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_CHAIN_1, (txTargetCode*)frame->v0);
			fxCoderAddBranch(coder, 1, XS_CODE_BRANCH_1, (txTargetCode*)frame->v1);
			fxCoderAdd(coder, 0, (txTargetCode*)frame->v0);
			fxCoderAddByte(coder, 0, XS_CODE_SWAP);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, coder->chainTarget);
			fxCoderAdd(coder, 0, (txTargetCode*)frame->v1);
			parser->codeThisResult = (txInteger)frame->i1;
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_MEMBER_AT_DELETE:
		/* stock: fxMemberAtNodeCodeDelete = fxMemberAtNodeCodeReference
		   (plain Code dispatch on reference AND at; AT only when not
		   super) + DELETE_SUPER_AT/DELETE_PROPERTY_AT. The previous
		   conversion used the Reference *dispatch* here (fxMemberNode
		   CodeReference emits only the base) and never dispatched the
		   key, so delete_property_at consumed unrelated stack slots:
		   jss StyleSheet.unregister silently deleted the wrong thing and
		   crashed the real page on a NULL base (probe35 repro). */
		switch (frame->stage) {
		case 0:
			frame->i0 = (((txMemberAtNode*)self)->reference->flags & mxSuperFlag) ? 1 : 0;
			frame->stage = 1;
			fxNodeDispatchCode(((txMemberAtNode*)self)->reference, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 1:
			frame->stage = 2;
			fxNodeDispatchCode(((txMemberAtNode*)self)->at, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 2:
			if (!frame->i0)
				fxCoderAddByte(coder, 0, (frame->i0) ? XS_CODE_SUPER_AT : XS_CODE_AT);
			fxCoderAddByte(coder, -1, (frame->i0) ? XS_CODE_DELETE_SUPER_AT : XS_CODE_DELETE_PROPERTY_AT);
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_MEMBER_AT_REFERENCE:
		/* stock: fxMemberAtNodeCodeReference — reference; at; AT when the
		   incoming flag is clear. Flag rides frame->stage (Reference
		   dispatch), so phase tracking lives in i0. */
		switch (frame->i0) {
		case 0:
			frame->i0 = 1;
			frame->i2 = frame->stage;
			fxNodeDispatchCode(((txMemberAtNode*)self)->reference, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 1:
			frame->i0 = 2;
			fxNodeDispatchCode(((txMemberAtNode*)self)->at, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		default:
			if (!frame->i2)
				fxCoderAddByte(coder, 0, (((txMemberAtNode*)self)->reference->flags & mxSuperFlag) ? XS_CODE_SUPER_AT : XS_CODE_AT);
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_MEMBER_AT_THIS:
		/* stock: fxMemberAtNodeCodeThis. flag path: reference; at; AT;
		   DUB_AT; GET_*_AT; result 2. plain path: reference; DUB; at; AT;
		   GET_*_AT; result 0. Flag rides frame->stage (This dispatch);
		   phase tracking lives in i0, flag saved to i2. */
		switch (frame->i0) {
		case 0:
			frame->i0 = 1;
			frame->i2 = frame->stage;
			fxNodeDispatchCode(((txMemberAtNode*)self)->reference, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 1:
			frame->i0 = 2;
			if (!frame->i2)
				fxCoderAddByte(coder, 1, XS_CODE_DUB);
			fxNodeDispatchCode(((txMemberAtNode*)self)->at, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		default:
			fxCoderAddByte(coder, 0, (((txMemberAtNode*)self)->reference->flags & mxSuperFlag) ? XS_CODE_SUPER_AT : XS_CODE_AT);
			if (frame->i2)
				fxCoderAddByte(coder, 2, XS_CODE_DUB_AT);
			fxCoderAddByte(coder, -1, (((txMemberAtNode*)self)->reference->flags & mxSuperFlag) ? XS_CODE_GET_SUPER_AT : XS_CODE_GET_PROPERTY_AT);
			parser->codeThisResult = (frame->i2) ? 2 : 0;
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_COMPOUND_NAME:
		/* stock: fxCompoundExpressionNodeCodeName — no child dispatches:
		   NAME symbol when the reference is an Access and the value is an
		   anonymous function/class. */
		if (((txAssignNode*)self)->reference->description->token == XS_TOKEN_ACCESS) {
			if (fxNodeCodeName(((txAssignNode*)self)->value))
				fxCoderAddSymbol(coder, 0, XS_CODE_NAME, ((txAccessNode*)((txAssignNode*)self)->reference)->symbol);
		}
		fxNodeWalkPopCode(parser);
		return;
	case WC_MEMBER_AT_ASSIGN:
		/* stock: fxMemberAtNodeCodeAssign — no dispatches; the Assign
		   dispatch flag rides frame->stage. */
		if (frame->stage)
			fxCoderAddByte(coder, 0, ((((txMemberAtNode*)self)->reference)->flags & mxSuperFlag) ? XS_CODE_SUPER_AT_2 : XS_CODE_AT_2);
		fxCoderAddByte(coder, -2, ((((txMemberAtNode*)self)->reference)->flags & mxSuperFlag) ? XS_CODE_SET_SUPER_AT : XS_CODE_SET_PROPERTY_AT);
		fxNodeWalkPopCode(parser);
		return;
	case WC_MEMBER_THIS:
		/* stock: fxMemberNodeCodeThis — reference; DUB; GET_SUPER/
		   GET_PROPERTY; result 1. Flag rides frame->stage; phase in i0. */
		switch (frame->i0) {
		case 0:
			frame->i0 = 1;
			fxNodeDispatchCode(((txMemberNode*)self)->reference, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		default:
			fxCoderAddByte(coder, 1, XS_CODE_DUB);
			fxCoderAddSymbol(coder, 0, ((((txMemberNode*)self)->reference)->flags & mxSuperFlag) ? XS_CODE_GET_SUPER : XS_CODE_GET_PROPERTY, ((txMemberNode*)self)->symbol);
			parser->codeThisResult = 1;
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_PRIVATE_MEMBER_THIS:
		/* stock: fxPrivateMemberNodeCodeThis — reference; DUB;
		   GET_PRIVATE_1; result 1. */
		switch (frame->i0) {
		case 0:
			frame->i0 = 1;
			fxNodeDispatchCode(((txPrivateMemberNode*)self)->reference, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		default:
			fxCoderAddByte(coder, 1, XS_CODE_DUB);
			fxCoderAddIndex(coder, 0, XS_CODE_GET_PRIVATE_1, ((txPrivateMemberNode*)self)->declaration->index);
			parser->codeThisResult = 1;
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_REGEXP:
		/* stock: fxRegexpNodeCode — REGEXP; NEW; modifier; value; RUN_1 2. */
		switch (frame->stage) {
		case 0:
			fxCoderAddByte(coder, 1, XS_CODE_REGEXP);
			fxCoderAddByte(coder, 2, XS_CODE_NEW);
			frame->stage = 1;
			fxNodeDispatchCode(((txRegexpNode*)self)->modifier, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 1:
			frame->stage = 2;
			fxNodeDispatchCode(((txRegexpNode*)self)->value, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		default:
			fxCoderAddInteger(coder, -4, XS_CODE_RUN_1, 2);
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_SUPER:
		/* stock: fxSuperNodeCode. stages: 0 = head (params dispatch when
		   the heritage is a class), 1 = params done, 2 = SET_THIS +
		   instance-init epilogue (shared tail). */
		switch (frame->stage) {
		case 0:
			if (coder->classNode->heritage->description->token == XS_TOKEN_HOST) {
				fxCoderAddByte(coder, 1, XS_CODE_TARGET);
				fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->prototypeSymbol);
				fxCoderAddByte(coder, 0, XS_CODE_INSTANTIATE);
				frame->stage = 2;
				goto wcs_tail;
			}
			fxCoderAddByte(coder, 3, XS_CODE_SUPER);
			frame->stage = 1;
			fxNodeDispatchCode(((txSuperNode*)self)->params, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 1:
			frame->stage = 2;
			/* fall through */
		case 2:
		wcs_tail:
			fxCoderAddByte(coder, 0, XS_CODE_SET_THIS);
			if (((txSuperNode*)self)->instanceInitAccess) {
				fxCoderAddByte(coder, 1, XS_CODE_GET_THIS);
				fxCoderAddIndex(coder, 1, XS_CODE_GET_CLOSURE_1, ((txSuperNode*)self)->instanceInitAccess->declaration->index);
				fxCoderAddByte(coder, 1, XS_CODE_CALL);
				fxCoderAddInteger(coder, -2, XS_CODE_RUN_1, 0);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
			}
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_BINDING:
		/* stock: fxBindingNodeCode. i0 = phase, i1 = error path (target
		   token is ACCESS: report + initializer dispatch only). Normal:
		   Reference(target,0); initializer; Assign(target,0); POP. */
		switch (frame->i0) {
		case 0: {
			txBindingNode* node = (txBindingNode*)self;
			frame->i1 = (node->target->description->token == XS_TOKEN_ACCESS) ? 1 : 0;
			if (frame->i1)
				fxReportParserError(coder->parser, self->line, "invalid initializer");
			frame->i0 = 1;
			if (frame->i1)
				fxNodeDispatchCode(node->initializer, coder);
			else
				fxNodeDispatchCodeReference(node->target, coder, 0);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		}
		case 1: {
			txBindingNode* node = (txBindingNode*)self;
			if (frame->i1) {
				fxNodeWalkPopCode(parser);
				break;
			}
			frame->i0 = 2;
			fxNodeDispatchCode(node->initializer, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		}
		case 2: {
			txBindingNode* node = (txBindingNode*)self;
			frame->i0 = 3;
			fxNodeDispatchCodeAssign(node->target, coder, 0);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		}
		default:
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_BINDING_ASSIGN:
		/* stock: fxBindingNodeCodeAssign — DUB/UNDEFINED/strict-!= guard,
		   POP, initializer dispatch, target patch, Assign(target, flag).
		   Flag rides frame->stage (Assign dispatch); phase in i0; guard
		   target in v0, flag saved to i1. */
		switch (frame->i0) {
		case 0: {
			txBindingNode* node = (txBindingNode*)self;
			frame->i1 = frame->stage;
			frame->v0 = fxCoderCreateTarget(coder);
			fxCoderAddByte(coder, 1, XS_CODE_DUB);
			fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
			fxCoderAddByte(coder, -1, XS_CODE_STRICT_NOT_EQUAL);
			fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, (txTargetCode*)frame->v0);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			frame->i0 = 1;
			fxNodeDispatchCode(node->initializer, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		}
		default: {
			/* xs_no_recursion: pop-before-epilogue — the final Assign dispatch
			   is a tail call. Popping AFTER it would pop the freshly pushed
			   child frame while running, and this frame would re-run its
			   default case forever. */
			txBindingNode* node = (txBindingNode*)self;
			txNode* target = node->target;
			txFlag flag = (txFlag)frame->i1;
			txTargetCode* guard = (txTargetCode*)frame->v0;
			fxNodeWalkPopCode(parser);
			fxCoderAdd(coder, 0, guard);
			fxNodeDispatchCodeAssign(target, coder, flag);
		}
		}
		return;
	case WC_BINDING_REFERENCE:
		/* stock: fxBindingNodeCodeReference — pass-through Reference
		   (target, flag). Flag rides frame->stage; phase in i0. */
		switch (frame->i0) {
		case 0:
			frame->i0 = 1;
			fxNodeDispatchCodeReference(((txBindingNode*)self)->target, coder, (txFlag)frame->stage);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		default:
			fxNodeWalkPopCode(parser);
		}
		return;
	case WC_PARAMS_BINDING:
		/* stock: fxParamsBindingNodeCode. Prologue (declaration) then one
		   item per loop: Reference(...,0); ARGUMENT(S); Assign(...,0); POP.
		   i0 = phase (0 head, 2/3 item resumes), i1 = argument index,
		   v0 = item cursor, i2 = declaration flag. */
		switch (frame->i0) {
		case 0: {
			txParamsBindingNode* node = (txParamsBindingNode*)self;
			frame->i1 = 0;
			frame->v0 = node->items ? node->items->first : C_NULL;
			frame->i2 = node->declaration ? 1 : 0;
			if (frame->i2) {
				fxCoderAddIndex(coder, 1, node->mapped ? XS_CODE_ARGUMENTS_SLOPPY : XS_CODE_ARGUMENTS_STRICT, node->items->length);
				if (node->declaration->flags & mxDeclareNodeClosureFlag)
					fxCoderAddIndex(coder, 0, XS_CODE_VAR_CLOSURE_1, node->declaration->index);
				else
					fxCoderAddIndex(coder, 0, XS_CODE_VAR_LOCAL_1, node->declaration->index);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
			}
			if (!frame->v0) {
				fxNodeWalkPopCode(parser);
				break;
			}
			frame->i0 = 1;
			goto wcpb_loop;
		}
		case 2:
			/* resume: Reference parked; emit the argument load */
			fxCoderAddIndex(coder, 1, (((txNode*)frame->v0)->description->token == XS_TOKEN_REST_BINDING) ? XS_CODE_ARGUMENTS : XS_CODE_ARGUMENT, (txInteger)frame->i1);
			frame->i0 = 3;
			if (((txNode*)frame->v0)->description->token == XS_TOKEN_REST_BINDING)
				fxNodeDispatchCodeAssign(((txRestBindingNode*)frame->v0)->binding, coder, 0);
			else
				fxNodeDispatchCodeAssign((txNode*)frame->v0, coder, 0);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 3:
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			frame->v0 = ((txNode*)frame->v0)->next;
			frame->i1 = (txInteger)frame->i1 + 1;
			frame->i0 = 1;
			goto wcpb_loop;
		}
		return;
	wcpb_loop:
		switch (frame->i0) {
		case 1:
			while ((txNode*)frame->v0) {
				txNode* item = (txNode*)frame->v0;
				frame->i0 = 2;
				if (item->description->token == XS_TOKEN_REST_BINDING)
					fxNodeDispatchCodeReference(((txRestBindingNode*)item)->binding, coder, 0);
				else
					fxNodeDispatchCodeReference(item, coder, 0);
				if (parser->nodeWalkStack != frame)
					return;
				/* inline tail mirrors the resume case */
				fxCoderAddIndex(coder, 1, (item->description->token == XS_TOKEN_REST_BINDING) ? XS_CODE_ARGUMENTS : XS_CODE_ARGUMENT, (txInteger)frame->i1);
				frame->i0 = 3;
				if (item->description->token == XS_TOKEN_REST_BINDING)
					fxNodeDispatchCodeAssign(((txRestBindingNode*)item)->binding, coder, 0);
				else
					fxNodeDispatchCodeAssign(item, coder, 0);
				if (parser->nodeWalkStack != frame)
					return;
				fxCoderAddByte(coder, -1, XS_CODE_POP);
				frame->v0 = item->next;
				frame->i1 = (txInteger)frame->i1 + 1;
				frame->i0 = 1;
			}
			fxNodeWalkPopCode(parser);
			break;
		}
		return;
	case WC_OBJECT_BINDING_ASSIGN:
		/* stock: fxObjectBindingNodeCodeAssign. Prologue (object temp,
		   TO_INSTANCE, optional COPY_OBJECT spread head), item loop,
		   optional rest tail, Unuse(2). i0 = phase (1 loop, 2/3 resumes,
		   4 at-resume), i1 = spread c counter, i2 = spread flag,
		   i3 = parked item kind (0 property, 1 at, 2 rest), v0 = cursor,
		   v1 = object temp, v2 = at temp. */
		switch (frame->i0) {
		case 0: {
			txObjectBindingNode* node = (txObjectBindingNode*)self;
			frame->i1 = 0;
			frame->i2 = (node->flags & mxSpreadFlag) ? 1 : 0;
			frame->v0 = node->items ? node->items->first : C_NULL;
			frame->v1 = (void*)fxCoderUseTemporaryVariable(coder);
			frame->v2 = (void*)fxCoderUseTemporaryVariable(coder);
			fxCoderAddByte(coder, 1, XS_CODE_DUB);
			fxCoderAddByte(coder, 0, XS_CODE_TO_INSTANCE);
			fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->v1);
			if (frame->i2) {
				fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
				fxCoderAddByte(coder, 1, XS_CODE_COPY_OBJECT);
				fxCoderAddByte(coder, 1, XS_CODE_CALL);
				fxCoderAddByte(coder, 1, XS_CODE_OBJECT);
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->v1);
				frame->i1 = 2;
			}
			frame->i0 = 1;
			goto wcoba_loop;
		}
		case 2:
			/* resume: Reference(binding,1) parked; finish per-kind tail */
			if (frame->i3 == 0) {
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->v1);
				fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, ((txPropertyBindingNode*)frame->v0)->symbol);
			}
			else if (frame->i3 == 1) {
				fxCoderAddByte(coder, 0, XS_CODE_AT);
				if (frame->i2) {
					fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->v2);
					fxCoderAddByte(coder, 0, XS_CODE_SWAP);
					fxCoderAddByte(coder, -1, XS_CODE_POP);
					frame->i1 = (txInteger)frame->i1 + 1;
				}
				else {
					fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->v2);
					fxCoderAddByte(coder, -1, XS_CODE_POP);
				}
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->v1);
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->v2);
				fxCoderAddByte(coder, -1, XS_CODE_GET_PROPERTY_AT);
			}
			else {
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->v1);
			}
			frame->i0 = 3;
			if (frame->i3 == 0)
				fxNodeDispatchCodeAssign(((txPropertyBindingNode*)frame->v0)->binding, coder, 1);
			else if (frame->i3 == 1)
				fxNodeDispatchCodeAssign(((txPropertyBindingAtNode*)frame->v0)->binding, coder, 1);
			else
				fxNodeDispatchCodeAssign(((txRestBindingNode*)frame->v0)->binding, coder, 1);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 3:
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			if (frame->i3 == 2) {
				fxCoderUnuseTemporaryVariables(coder, 2);
				fxNodeWalkPopCode(parser);
				break;
			}
			frame->v0 = ((txNode*)frame->v0)->next;
			frame->i0 = 1;
			goto wcoba_loop;
		case 4:
			/* resume: at-expression parked; emit AT + at-temp bookkeeping */
			fxCoderAddByte(coder, 0, XS_CODE_AT);
			if (frame->i2) {
				fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->v2);
				fxCoderAddByte(coder, 0, XS_CODE_SWAP);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
				frame->i1 = (txInteger)frame->i1 + 1;
			}
			else {
				fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->v2);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
			}
			frame->i0 = 2;
			fxNodeDispatchCodeReference(((txPropertyBindingAtNode*)frame->v0)->binding, coder, 1);
			if (parser->nodeWalkStack != frame)
				return;
			/* inline tail mirrors the resume case (kind 1) */
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->v1);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->v2);
			fxCoderAddByte(coder, -1, XS_CODE_GET_PROPERTY_AT);
			frame->i0 = 3;
			fxNodeDispatchCodeAssign(((txPropertyBindingAtNode*)frame->v0)->binding, coder, 1);
			if (parser->nodeWalkStack != frame)
				return;
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			frame->v0 = ((txNode*)frame->v0)->next;
			frame->i0 = 1;
			goto wcoba_loop;
		}
		return;
	wcoba_loop:
		while ((txNode*)frame->v0 && ((txNode*)frame->v0)->description->token != XS_TOKEN_REST_BINDING) {
			txNode* item = (txNode*)frame->v0;
			if (item->description->token == XS_TOKEN_PROPERTY_BINDING) {
				if (frame->i2) {
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->v1);
					fxCoderAddSymbol(coder, 1, XS_CODE_SYMBOL, ((txPropertyBindingNode*)item)->symbol);
					fxCoderAddByte(coder, 0, XS_CODE_AT);
					fxCoderAddByte(coder, 0, XS_CODE_SWAP);
					fxCoderAddByte(coder, -1, XS_CODE_POP);
					frame->i1 = (txInteger)frame->i1 + 1;
				}
				frame->i3 = 0;
				frame->i0 = 2;
				fxNodeDispatchCodeReference(((txPropertyBindingNode*)item)->binding, coder, 1);
				if (parser->nodeWalkStack != frame)
					return;
				/* inline tail mirrors resume case 2 (kind 0) */
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->v1);
				fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, ((txPropertyBindingNode*)item)->symbol);
				frame->i0 = 3;
				fxNodeDispatchCodeAssign(((txPropertyBindingNode*)item)->binding, coder, 1);
				if (parser->nodeWalkStack != frame)
					return;
				fxCoderAddByte(coder, -1, XS_CODE_POP);
				frame->v0 = item->next;
				frame->i0 = 1;
				continue;
			}
			else {
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->v1);
				frame->i3 = 1;
				frame->i0 = 4;
				fxNodeDispatchCode(((txPropertyBindingAtNode*)item)->at, coder);
				if (parser->nodeWalkStack != frame)
					return;
				/* inline tail mirrors resume case 4 */
				fxCoderAddByte(coder, 0, XS_CODE_AT);
				if (frame->i2) {
					fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->v2);
					fxCoderAddByte(coder, 0, XS_CODE_SWAP);
					fxCoderAddByte(coder, -1, XS_CODE_POP);
					frame->i1 = (txInteger)frame->i1 + 1;
				}
				else {
					fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->v2);
					fxCoderAddByte(coder, -1, XS_CODE_POP);
				}
				frame->i0 = 2;
				fxNodeDispatchCodeReference(((txPropertyBindingAtNode*)item)->binding, coder, 1);
				if (parser->nodeWalkStack != frame)
					return;
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->v1);
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->v2);
				fxCoderAddByte(coder, -1, XS_CODE_GET_PROPERTY_AT);
				frame->i0 = 3;
				fxNodeDispatchCodeAssign(((txPropertyBindingAtNode*)item)->binding, coder, 1);
				if (parser->nodeWalkStack != frame)
					return;
				fxCoderAddByte(coder, -1, XS_CODE_POP);
				frame->v0 = item->next;
				frame->i0 = 1;
				continue;
			}
		}
		if (frame->i2 && (txNode*)frame->v0) {
			/* rest tail */
			fxCoderAddInteger(coder, -2 - (txInteger)frame->i1, XS_CODE_RUN_1, (txInteger)frame->i1);
			fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->v1);
			frame->i3 = 2;
			frame->i0 = 2;
			fxNodeDispatchCodeReference(((txRestBindingNode*)frame->v0)->binding, coder, 1);
			if (parser->nodeWalkStack != frame)
				return;
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->v1);
			frame->i0 = 3;
			fxNodeDispatchCodeAssign(((txRestBindingNode*)frame->v0)->binding, coder, 1);
			if (parser->nodeWalkStack != frame)
				return;
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			fxCoderUnuseTemporaryVariables(coder, 2);
			fxNodeWalkPopCode(parser);
			break;
		}
		fxCoderUnuseTemporaryVariables(coder, 2);
		fxNodeWalkPopCode(parser);
		break;
	case WC_DELEGATE:
		/* stock: fxDelegateNodeCode (yield*). One child dispatch (the
		   expression), then straight-line iterator-protocol encoding.
		   i0=async, i1..i4=iterator/method/next/result temps, v0..v5=
		   next/catch/rethrow/return/normal/done targets. */
		switch (frame->stage) {
		case 0: {
			txStatementNode* node = (txStatementNode*)self;
			frame->i0 = (self->flags & mxAsyncFlag) ? 1 : 0;
			frame->v0 = fxCoderCreateTarget(coder);
			frame->v1 = fxCoderCreateTarget(coder);
			frame->v2 = fxCoderCreateTarget(coder);
			frame->v3 = fxCoderCreateTarget(coder);
			frame->v4 = fxCoderCreateTarget(coder);
			frame->v5 = fxCoderCreateTarget(coder);
			frame->i1 = fxCoderUseTemporaryVariable(coder);
			frame->i2 = fxCoderUseTemporaryVariable(coder);
			frame->i3 = fxCoderUseTemporaryVariable(coder);
			frame->i4 = fxCoderUseTemporaryVariable(coder);
			frame->stage = 1;
			fxNodeDispatchCode(node->expression, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		}
		case 1: {
			txInteger iterator = (txInteger)frame->i1;
			txInteger method = (txInteger)frame->i2;
			txInteger next = (txInteger)frame->i3;
			txInteger result = (txInteger)frame->i4;
			txInteger async = (txInteger)frame->i0;
			txTargetCode* nextTarget = (txTargetCode*)frame->v0;
			txTargetCode* catchTarget = (txTargetCode*)frame->v1;
			txTargetCode* rethrowTarget = (txTargetCode*)frame->v2;
			txTargetCode* returnTarget = (txTargetCode*)frame->v3;
			txTargetCode* normalTarget = (txTargetCode*)frame->v4;
			txTargetCode* doneTarget = (txTargetCode*)frame->v5;
			fxCoderAddByte(coder, 0, async ? XS_CODE_FOR_AWAIT_OF : XS_CODE_FOR_OF);
			fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, iterator);
			fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->nextSymbol);
			fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, next);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
			fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, result);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			fxCoderAddBranch(coder, 0, XS_CODE_CATCH_1, catchTarget);
			fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, normalTarget);
			/* LOOP */
			fxCoderAdd(coder, 0, nextTarget);
			if (async)
				fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->valueSymbol);
			fxCoderAddByte(coder, 0, XS_CODE_YIELD_STAR);
			fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, result);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			fxCoderAddBranch(coder, 0, XS_CODE_CATCH_1, catchTarget);
			fxCoderAddBranch(coder, 1, XS_CODE_BRANCH_STATUS_1, normalTarget);
			/* RETURN */
			fxCoderAddByte(coder, 0, XS_CODE_UNCATCH);
			if (async) {
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, result);
				fxCoderAddByte(coder, 0, XS_CODE_AWAIT);
				fxCoderAddByte(coder, 0, XS_CODE_THROW_STATUS);
				fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, result);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
			}
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, iterator);
			fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->returnSymbol);
			fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_CHAIN_1, returnTarget);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, iterator);
			fxCoderAddByte(coder, 0, XS_CODE_SWAP);
			fxCoderAddByte(coder, 1, XS_CODE_CALL);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, result);
			fxCoderAddInteger(coder, -3, XS_CODE_RUN_1, 1);
			if (async) {
				fxCoderAddByte(coder, 0, XS_CODE_AWAIT);
				fxCoderAddByte(coder, 0, XS_CODE_THROW_STATUS);
			}
			fxCoderAddByte(coder, 0, XS_CODE_CHECK_INSTANCE);
			fxCoderAddByte(coder, 1, XS_CODE_DUB);
			fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->doneSymbol);
			fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_ELSE_1, nextTarget);
			fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->valueSymbol);
			fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, result);
			fxCoderAdd(coder, 0, returnTarget);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, result);
			if (async) {
				fxCoderAddByte(coder, 0, XS_CODE_AWAIT);
				fxCoderAddByte(coder, 0, XS_CODE_THROW_STATUS);
			}
			fxCoderAddByte(coder, -1, XS_CODE_SET_RESULT);
			fxCoderAdjustEnvironment(coder, coder->returnTarget);
			fxCoderAdjustScope(coder, coder->returnTarget);
			fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, coder->returnTarget);
			/* THROW */
			fxCoderAdd(coder, 0, catchTarget);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, iterator);
			fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->throwSymbol);
			fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, method);
			fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_COALESCE_1, doneTarget);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, iterator);
			fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->returnSymbol);
			fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_CHAIN_1, rethrowTarget);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, iterator);
			fxCoderAddByte(coder, 0, XS_CODE_SWAP);
			fxCoderAddByte(coder, 1, XS_CODE_CALL);
			fxCoderAddInteger(coder, -2, XS_CODE_RUN_1, 0);
			if (async) {
				fxCoderAddByte(coder, 0, XS_CODE_AWAIT);
				fxCoderAddByte(coder, 0, XS_CODE_THROW_STATUS);
			}
			fxCoderAddByte(coder, 0, XS_CODE_CHECK_INSTANCE);
			fxCoderAdd(coder, 0, rethrowTarget);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
			fxCoderAddByte(coder, 0, XS_CODE_CHECK_INSTANCE);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			/* NORMAL */
			fxCoderAdd(coder, 0, normalTarget);
			fxCoderAddByte(coder, 0, XS_CODE_UNCATCH);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, next);
			fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, method);
			fxCoderAdd(coder, 1, doneTarget);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, iterator);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, method);
			fxCoderAddByte(coder, 1, XS_CODE_CALL);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, result);
			fxCoderAddInteger(coder, -3, XS_CODE_RUN_1, 1);
			if (async) {
				fxCoderAddByte(coder, 0, XS_CODE_AWAIT);
				fxCoderAddByte(coder, 0, XS_CODE_THROW_STATUS);
			}
			fxCoderAddByte(coder, 0, XS_CODE_CHECK_INSTANCE);
			fxCoderAddByte(coder, 1, XS_CODE_DUB);
			fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->doneSymbol);
			fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_ELSE_1, nextTarget);
			fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->valueSymbol);
			fxCoderUnuseTemporaryVariables(coder, 4);
			fxNodeWalkPopCode(parser);
		}
		}
		return;
	case WC_ARRAY_BINDING_ASSIGN:
		/* stock: fxArrayBindingNodeCodeAssign — for-of protocol over the
		   pattern items. i0..i5 = iterator/next/done/selector/rest/result
		   temps, i7 = phase, v0 = item cursor, v1..v3 = catch/normal/
		   finally targets, v4/v5/v6 = step/done/next targets. */
		switch (frame->i7) {
		case 0: {
			txArrayBindingNode* node = (txArrayBindingNode*)self;
			frame->i0 = fxCoderUseTemporaryVariable(coder);
			frame->i1 = fxCoderUseTemporaryVariable(coder);
			frame->i2 = fxCoderUseTemporaryVariable(coder);
			frame->i3 = fxCoderUseTemporaryVariable(coder);
			frame->i4 = fxCoderUseTemporaryVariable(coder);
			frame->i5 = fxCoderUseTemporaryVariable(coder);
			coder->returnTarget = fxCoderAliasTargets(coder, coder->returnTarget);
			frame->v1 = fxCoderCreateTarget(coder);
			frame->v2 = fxCoderCreateTarget(coder);
			frame->v3 = fxCoderCreateTarget(coder);
			fxCoderAddByte(coder, 1, XS_CODE_DUB);
			fxCoderAddByte(coder, 0, XS_CODE_FOR_OF);
			fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i0);
			fxCoderAddByte(coder, 1, XS_CODE_FALSE);
			fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i2);
			fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, 0);
			fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i3);
			fxCoderAddBranch(coder, 0, XS_CODE_CATCH_1, (txTargetCode*)frame->v1);
			frame->v0 = node->items ? node->items->first : C_NULL;
			if (frame->v0) {
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i0);
				fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->nextSymbol);
				fxCoderAddIndex(coder, 0, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i1);
			}
			frame->i7 = 1;
			goto wcaba_loop;
		}
		case 2:
			/* resume: element Reference(item,1) parked; mid-block */
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i2);
			fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, (txTargetCode*)frame->v4);
			fxCoderAddByte(coder, 1, XS_CODE_TRUE);
			fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i2);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i0);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i1);
			fxCoderAddByte(coder, 1, XS_CODE_CALL);
			fxCoderAddInteger(coder, -2, XS_CODE_RUN_1, 0);
			fxCoderAddByte(coder, 0, XS_CODE_CHECK_INSTANCE);
			fxCoderAddByte(coder, 1, XS_CODE_DUB);
			fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->doneSymbol);
			fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->i2);
			fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, (txTargetCode*)frame->v5);
			fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->valueSymbol);
			fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, (txTargetCode*)frame->v6);
			fxCoderAdd(coder, 1, (txTargetCode*)frame->v5);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			fxCoderAdd(coder, 1, (txTargetCode*)frame->v4);
			fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
			fxCoderAdd(coder, 1, (txTargetCode*)frame->v6);
			frame->i7 = 3;
			fxNodeDispatchCodeAssign((txNode*)frame->v0, coder, 1);
			if (parser->nodeWalkStack != frame)
				return;
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			frame->v0 = ((txNode*)frame->v0)->next;
			frame->i7 = 1;
			goto wcaba_loop;
		case 3:
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			frame->v0 = ((txNode*)frame->v0)->next;
			frame->i7 = 1;
			goto wcaba_loop;
		case 4:
			/* resume: rest Reference(binding,1) parked; rest mid-block */
			fxCoderAddByte(coder, 1, XS_CODE_ARRAY);
			fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i4);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i2);
			fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, (txTargetCode*)frame->v5);
			fxCoderAdd(coder, 0, (txTargetCode*)frame->v6);
			fxCoderAddByte(coder, 1, XS_CODE_TRUE);
			fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i2);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i0);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i1);
			fxCoderAddByte(coder, 1, XS_CODE_CALL);
			fxCoderAddInteger(coder, -2, XS_CODE_RUN_1, 0);
			fxCoderAddByte(coder, 0, XS_CODE_CHECK_INSTANCE);
			fxCoderAddIndex(coder, 1, XS_CODE_SET_LOCAL_1, (txInteger)frame->i5);
			fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->doneSymbol);
			fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->i2);
			fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, (txTargetCode*)frame->v5);
			fxCoderAddIndex(coder, 0, XS_CODE_GET_LOCAL_1, (txInteger)frame->i4);
			fxCoderAddByte(coder, 1, XS_CODE_DUB);
			fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->lengthSymbol);
			fxCoderAddByte(coder, 0, XS_CODE_AT);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i5);
			fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->valueSymbol);
			fxCoderAddByte(coder, -2, XS_CODE_SET_PROPERTY_AT);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, (txTargetCode*)frame->v6);
			fxCoderAdd(coder, 1, (txTargetCode*)frame->v5);
			fxCoderAddIndex(coder, 0, XS_CODE_GET_LOCAL_1, (txInteger)frame->i4);
			frame->i7 = 5;
			fxNodeDispatchCodeAssign(((txRestBindingNode*)frame->v0)->binding, coder, 1);
			if (parser->nodeWalkStack != frame)
				return;
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			goto wcaba_tail;
		case 5:
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			goto wcaba_tail;
		}
		return;
	wcaba_loop:
		while ((txNode*)frame->v0 && ((txNode*)frame->v0)->description->token != XS_TOKEN_REST_BINDING) {
			txNode* item = (txNode*)frame->v0;
			frame->v4 = fxCoderCreateTarget(coder);
			if (item->description->token == XS_TOKEN_SKIP_BINDING) {
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i2);
				fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, (txTargetCode*)frame->v4);
				fxCoderAddByte(coder, 1, XS_CODE_TRUE);
				fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i2);
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i0);
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i1);
				fxCoderAddByte(coder, 1, XS_CODE_CALL);
				fxCoderAddInteger(coder, -2, XS_CODE_RUN_1, 0);
				fxCoderAddByte(coder, 0, XS_CODE_CHECK_INSTANCE);
				fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->doneSymbol);
				fxCoderAddIndex(coder, 0, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i2);
				fxCoderAdd(coder, 1, (txTargetCode*)frame->v4);
				frame->v0 = item->next;
				continue;
			}
			frame->v5 = fxCoderCreateTarget(coder);
			frame->v6 = fxCoderCreateTarget(coder);
			frame->i7 = 2;
			fxNodeDispatchCodeReference(item, coder, 1);
			if (parser->nodeWalkStack != frame)
				return;
			/* inline tail mirrors resume case 2 */
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i2);
			fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, (txTargetCode*)frame->v4);
			fxCoderAddByte(coder, 1, XS_CODE_TRUE);
			fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i2);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i0);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i1);
			fxCoderAddByte(coder, 1, XS_CODE_CALL);
			fxCoderAddInteger(coder, -2, XS_CODE_RUN_1, 0);
			fxCoderAddByte(coder, 0, XS_CODE_CHECK_INSTANCE);
			fxCoderAddByte(coder, 1, XS_CODE_DUB);
			fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->doneSymbol);
			fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->i2);
			fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, (txTargetCode*)frame->v5);
			fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->valueSymbol);
			fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, (txTargetCode*)frame->v6);
			fxCoderAdd(coder, 1, (txTargetCode*)frame->v5);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			fxCoderAdd(coder, 1, (txTargetCode*)frame->v4);
			fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
			fxCoderAdd(coder, 1, (txTargetCode*)frame->v6);
			frame->i7 = 3;
			fxNodeDispatchCodeAssign(item, coder, 1);
			if (parser->nodeWalkStack != frame)
				return;
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			frame->v0 = item->next;
			frame->i7 = 1;
		}
		if ((txNode*)frame->v0) {
			frame->v6 = fxCoderCreateTarget(coder);
			frame->v5 = fxCoderCreateTarget(coder);
			frame->i7 = 4;
			fxNodeDispatchCodeReference(((txRestBindingNode*)frame->v0)->binding, coder, 1);
			if (parser->nodeWalkStack != frame)
				return;
			/* inline tail mirrors resume case 4 */
			fxCoderAddByte(coder, 1, XS_CODE_ARRAY);
			fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i4);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i2);
			fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, (txTargetCode*)frame->v5);
			fxCoderAdd(coder, 0, (txTargetCode*)frame->v6);
			fxCoderAddByte(coder, 1, XS_CODE_TRUE);
			fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i2);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i0);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i1);
			fxCoderAddByte(coder, 1, XS_CODE_CALL);
			fxCoderAddInteger(coder, -2, XS_CODE_RUN_1, 0);
			fxCoderAddByte(coder, 0, XS_CODE_CHECK_INSTANCE);
			fxCoderAddIndex(coder, 1, XS_CODE_SET_LOCAL_1, (txInteger)frame->i5);
			fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->doneSymbol);
			fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->i2);
			fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, (txTargetCode*)frame->v5);
			fxCoderAddIndex(coder, 0, XS_CODE_GET_LOCAL_1, (txInteger)frame->i4);
			fxCoderAddByte(coder, 1, XS_CODE_DUB);
			fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->lengthSymbol);
			fxCoderAddByte(coder, 0, XS_CODE_AT);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i5);
			fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->valueSymbol);
			fxCoderAddByte(coder, -2, XS_CODE_SET_PROPERTY_AT);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, (txTargetCode*)frame->v6);
			fxCoderAdd(coder, 1, (txTargetCode*)frame->v5);
			fxCoderAddIndex(coder, 0, XS_CODE_GET_LOCAL_1, (txInteger)frame->i4);
			frame->i7 = 5;
			fxNodeDispatchCodeAssign(((txRestBindingNode*)frame->v0)->binding, coder, 1);
			if (parser->nodeWalkStack != frame)
				return;
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			goto wcaba_tail;
		}
		/* fall into the shared tail */
	wcaba_tail:
		{
			txInteger selection = 1;
			txTargetCode* nextTarget;
			txTargetCode* doneTarget;
			txTargetCode* returnTarget;
			fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, (txTargetCode*)frame->v2);
			coder->returnTarget = fxCoderFinalizeTargets(coder, coder->returnTarget, (txInteger)frame->i3, &selection, (txTargetCode*)frame->v3);
			fxCoderAdd(coder, 0, (txTargetCode*)frame->v2);
			fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, selection);
			fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->i3);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			fxCoderAdd(coder, 0, (txTargetCode*)frame->v3);
			fxCoderAddByte(coder, 0, XS_CODE_UNCATCH);
			fxCoderAdd(coder, 0, (txTargetCode*)frame->v1);
			nextTarget = fxCoderCreateTarget(coder);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i3);
			fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, nextTarget);
			fxCoderAddByte(coder, 1, XS_CODE_EXCEPTION);
			fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->i5);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			frame->v1 = fxCoderCreateTarget(coder);
			fxCoderAddBranch(coder, 0, XS_CODE_CATCH_1, (txTargetCode*)frame->v1);
			fxCoderAdd(coder, 0, nextTarget);
			doneTarget = fxCoderCreateTarget(coder);
			returnTarget = fxCoderCreateTarget(coder);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i2);
			fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, doneTarget);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i0);
			fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->returnSymbol);
			fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_CHAIN_1, returnTarget);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i0);
			fxCoderAddByte(coder, 0, XS_CODE_SWAP);
			fxCoderAddByte(coder, 1, XS_CODE_CALL);
			fxCoderAddInteger(coder, -2, XS_CODE_RUN_1, 0);
			fxCoderAddByte(coder, 0, XS_CODE_CHECK_INSTANCE);
			fxCoderAdd(coder, 0, returnTarget);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			fxCoderAdd(coder, 0, doneTarget);
			nextTarget = fxCoderCreateTarget(coder);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i3);
			fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, nextTarget);
			fxCoderAddByte(coder, 0, XS_CODE_UNCATCH);
			fxCoderAdd(coder, 0, (txTargetCode*)frame->v1);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i5);
			fxCoderAddByte(coder, -1, XS_CODE_THROW);
			fxCoderAdd(coder, 0, nextTarget);
			selection = 1;
			fxCoderJumpTargets(coder, coder->returnTarget, (txInteger)frame->i3, &selection);
			fxCoderUnuseTemporaryVariables(coder, 6);
		}
		fxNodeWalkPopCode(parser);
		return;
	/* R7/B5a: stock fxArrayNodeCode. v0=array temp, v1=item cursor,
	   i0=spread flag, i1=counter temp (spread) or element index (plain),
	   i2=elision flag, v2/v3/v4 = parked spread item/nextTarget/doneTarget,
	   i4/i5 = iterator/result temps. stages: 0 prologue, 1 loop,
	   2 spread-expression resume, 3 element resume. */
	case WC_ARRAY: {

		txArrayNode* node = (txArrayNode*)self;
		switch (frame->stage) {
		case 0:
			frame->v0 = (void*)fxCoderUseTemporaryVariable(coder);
			fxCoderAddByte(coder, 1, XS_CODE_ARRAY);
			fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->v0);
			if (node->items) {
				txNode* item = node->items->first;
				frame->i0 = (node->flags & mxSpreadFlag) ? 1 : 0;
				frame->v1 = item;
				if (frame->i0) {
					frame->i1 = fxCoderUseTemporaryVariable(coder);
					fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, 0);
					fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i1);
				}
				else {
					txNode* scan = item;
					while (scan) {
						if (scan->description->token == XS_TOKEN_ELISION) {
							frame->i2 = 1;
							break;
						}
						scan = scan->next;
					}
					if (!frame->i2) {
						fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->v0);
						fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, node->items->length);
						fxCoderAddSymbol(coder, -1, XS_CODE_SET_PROPERTY, coder->parser->lengthSymbol);
						fxCoderAddByte(coder, -1, XS_CODE_POP);
					}
					frame->i1 = 0; /* element index */
				}
			}
			frame->stage = 1;
			goto wcarray_loop;
		case 1:
			goto wcarray_loop;
		case 2: /* resume: spread expression parked; finish the spread item */
			{
				txNode* item = (txNode*)frame->v2;
				txInteger iterator = (txInteger)frame->i4;
				txInteger result = (txInteger)frame->i5;
				txInteger array = (txInteger)frame->v0;
				txInteger counter = (txInteger)frame->i1;
				txTargetCode* nextTarget = (txTargetCode*)frame->v3;
				txTargetCode* doneTarget = (txTargetCode*)frame->v4;
				fxCoderAddByte(coder, 0, XS_CODE_FOR_OF);
				fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, iterator);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
				fxCoderAdd(coder, 0, nextTarget);
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, iterator);
				fxCoderAddByte(coder, 1, XS_CODE_DUB);
				fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->nextSymbol);
				fxCoderAddByte(coder, 1, XS_CODE_CALL);
				fxCoderAddInteger(coder, -2, XS_CODE_RUN_1, 0);
				fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, result);
				fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->doneSymbol);
				fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, doneTarget);
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, array);
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, counter);
				fxCoderAddByte(coder, 0, XS_CODE_AT);
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, result);
				fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->valueSymbol);
				fxCoderAddByte(coder, -2, XS_CODE_SET_PROPERTY_AT);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, counter);
				fxCoderAddByte(coder, 0, XS_CODE_INCREMENT);
				fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, counter);
				fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, nextTarget);
				fxCoderAdd(coder, 1, doneTarget);
				fxCoderUnuseTemporaryVariables(coder, 2);
				frame->v1 = item->next;
				frame->stage = 1;
				goto wcarray_loop;
			}
		case 3: /* resume: non-spread element parked (index in i1) */
			fxCoderAddByte(coder, -3, XS_CODE_NEW_PROPERTY_AT);
			fxCoderAddInteger(coder, 0, XS_CODE_INTEGER_1, 0);
			/* mirror the skipped tail of the parked iteration: advance the
			   element index and the cursor (fxNodeWalkStack == frame here) */
			frame->i1 = frame->i1 + 1;
			frame->v1 = ((txNode*)frame->v1)->next;
			frame->stage = 1;
			goto wcarray_loop;
		case 4: /* resume: spread-path element parked (counter in i1) */
			fxCoderAddByte(coder, -2, XS_CODE_SET_PROPERTY_AT);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i1);
			fxCoderAddByte(coder, 0, XS_CODE_INCREMENT);
			fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i1);
			/* mirror the skipped cursor advance of the parked iteration */
			frame->v1 = ((txNode*)frame->v1)->next;
			frame->stage = 1;
			goto wcarray_loop;
		}
		return;
	wcarray_loop:
		while (frame->v1) {
			txNode* item = (txNode*)frame->v1;
			if (frame->i0) {
				/* spread path */
				if (item->description->token == XS_TOKEN_SPREAD) {
					frame->v2 = item;
					frame->v3 = fxCoderCreateTarget(coder);
					frame->v4 = fxCoderCreateTarget(coder);
					frame->i4 = fxCoderUseTemporaryVariable(coder);
					frame->i5 = fxCoderUseTemporaryVariable(coder);
					fxNodeDispatchCode(((txSpreadNode*)item)->expression, coder);
					if (parser->nodeWalkStack != frame) {
						frame->stage = 2;
						return;
					}
					/* inline tail mirrors stage 2 (cursor not advanced yet) */
					{
					txInteger iterator = (txInteger)frame->i4;
					txInteger result = (txInteger)frame->i5;
					txInteger array = (txInteger)frame->v0;
					txInteger counter = (txInteger)frame->i1;
					fxCoderAddByte(coder, 0, XS_CODE_FOR_OF);
					fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, iterator);
					fxCoderAddByte(coder, -1, XS_CODE_POP);
					fxCoderAdd(coder, 0, (txTargetCode*)frame->v3);
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, iterator);
					fxCoderAddByte(coder, 1, XS_CODE_DUB);
					fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->nextSymbol);
					fxCoderAddByte(coder, 1, XS_CODE_CALL);
					fxCoderAddInteger(coder, -2, XS_CODE_RUN_1, 0);
					fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, result);
					fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->doneSymbol);
					fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, (txTargetCode*)frame->v4);
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, array);
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, counter);
					fxCoderAddByte(coder, 0, XS_CODE_AT);
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, result);
					fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->valueSymbol);
					fxCoderAddByte(coder, -2, XS_CODE_SET_PROPERTY_AT);
					fxCoderAddByte(coder, -1, XS_CODE_POP);
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, counter);
					fxCoderAddByte(coder, 0, XS_CODE_INCREMENT);
					fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, counter);
					fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, (txTargetCode*)frame->v3);
					fxCoderAdd(coder, 1, (txTargetCode*)frame->v4);
					fxCoderUnuseTemporaryVariables(coder, 2);
					}
					frame->v1 = item->next;
					continue;
				}
				else if (item->description->token != XS_TOKEN_ELISION) {
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->v0);
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i1);
					fxCoderAddByte(coder, 0, XS_CODE_AT);
					fxNodeDispatchCode(item, coder);
					if (parser->nodeWalkStack != frame) {
						frame->stage = 4;
						return;
					}
					fxCoderAddByte(coder, -2, XS_CODE_SET_PROPERTY_AT);
					fxCoderAddByte(coder, -1, XS_CODE_POP);
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i1);
					fxCoderAddByte(coder, 0, XS_CODE_INCREMENT);
					fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, (txInteger)frame->i1);
				}
				else {
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->v0);
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i1);
					fxCoderAddByte(coder, 0, XS_CODE_INCREMENT);
					fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->i1);
					fxCoderAddSymbol(coder, -1, XS_CODE_SET_PROPERTY, coder->parser->lengthSymbol);
					fxCoderAddByte(coder, -1, XS_CODE_POP);
				}
				frame->v1 = item->next;
			}
			else {
				/* non-spread path */
				if (item->description->token != XS_TOKEN_ELISION) {
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->v0);
					fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, (int)frame->i1);
					fxCoderAddByte(coder, 0, XS_CODE_AT);
					fxNodeDispatchCode(item, coder);
					if (parser->nodeWalkStack != frame) {
						frame->stage = 3;
						return;
					}
					fxCoderAddByte(coder, -3, XS_CODE_NEW_PROPERTY_AT);
					fxCoderAddInteger(coder, 0, XS_CODE_INTEGER_1, 0);
				}
				frame->i1 = frame->i1 + 1;
				frame->v1 = item->next;
			}
		}
		if (frame->i0)
			fxCoderUnuseTemporaryVariables(coder, 1); /* counter */
		fxCoderUnuseTemporaryVariables(coder, 1); /* array */
		fxNodeWalkPopCode(parser);
		return;
	}
	/* R7/B5a: stock fxFunctionNodeCode. Saves/restores coder fields and
	   redirects emission into a nested function body.
	   v0=target, v1=name symbol, v2=returnTarget, v3=former name symbol;
	   i0..i7 = saved environmentLevel/evalFlag/line/programFlag/scopeLevel/
	   firstBreakTarget/firstContinueTarget/returnTarget;
	   v4..v7 = saved firstBreakTarget/firstContinueTarget/returnTarget ptrs.
	   stages: 0 prologue+header, 1 after-params, 2 after-body,
	   3 environment epilogue. */
	case WC_DECLARE:
		/* stock: fxDeclareNodeCode. CONST/USING error only; LET emits
		   Reference(self,0) / UNDEFINED / Assign(self,0) / POP. All the
		   child "dispatches" target self via the DECLARE_REFERENCE /
		   DECLARE_ASSIGN variants, so each parks this frame at its
		   dedicated stage before resuming here. */
		switch (frame->stage) {
		case 0:
			if (self->description->token == XS_TOKEN_CONST) {
				fxReportParserError(coder->parser, self->line, "invalid const");
				fxNodeWalkPopCode(parser);
				return;
			}
			else if (self->description->token == XS_TOKEN_USING) {
				fxReportParserError(coder->parser, self->line, "invalid using");
				fxNodeWalkPopCode(parser);
				return;
			}
			else if (self->description->token == XS_TOKEN_LET) {
				frame->stage = 1;
				fxNodeDispatchCodeReference(self, coder, 0);
				if (parser->nodeWalkStack != frame)
					return;
				/* fall through */
			}
			else {
				fxNodeWalkPopCode(parser);
				return;
			}
		case 1:
			if (frame->stage == 1) {
				frame->stage = 2;
				fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
				fxNodeDispatchCodeAssign(self, coder, 0);
				if (parser->nodeWalkStack != frame)
					return;
				/* fall through */
			}
			/* fall through */
		case 2:
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			fxNodeWalkPopCode(parser);
			return;
		}
		return;
	case WC_DECLARE_ASSIGN:
		/* stock: fxDeclareNodeCodeAssign — pure emission, no dispatches:
		   writes the SET_ op for self->declaration and pops. flag unused. */
		fxDeclareNodeCodeAssign(self, coder, (txFlag)frame->stage);
		fxNodeWalkPopCode(parser);
		return;
	case WC_DECLARE_REFERENCE:
		/* stock: fxDeclareNodeCodeReference — emission only. */
		fxDeclareNodeCodeReference(self, coder, (txFlag)frame->stage);
		fxNodeWalkPopCode(parser);
		return;
	case WC_DEFINE:
		/* stock: fxDefineNodeCode — CodedFlag guard, Reference(self,0),
		   initializer dispatch (then nulled), Assign(self,0), POP.
		   i0 = node->initializer parked across stage 1. */
		switch (frame->stage) {
		case 0:
			if (((txDefineNode*)self)->flags & mxDefineNodeCodedFlag) {
				fxNodeWalkPopCode(parser);
				return;
			}
			((txDefineNode*)self)->flags |= mxDefineNodeCodedFlag;
			frame->stage = 1;
			fxDeclareNodeCodeReference(self, coder, 0);
			frame->v0 = ((txDefineNode*)self)->initializer;
			((txDefineNode*)self)->initializer = C_NULL;
			fxNodeDispatchCode((txNode*)frame->v0, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 1:
			fxDeclareNodeCodeAssign(self, coder, 0);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			fxNodeWalkPopCode(parser);
			return;
		}
		return;
	case WC_FIELD: {
		/* stock: fxFieldNodeCode — THIS; PROPERTY: value dispatch,
		   NEW_PROPERTY symbol, NAME flag integer. PROPERTY_AT: closure of
		   at declaration, value dispatch, NEW_PROPERTY_AT, flag. PRIVATE:
		   closure/method path or value dispatch, NEW_PRIVATE_1, flag. Only
		   the value dispatch parks (stage 1). i0 = item kind, i1 = name
		   flag scratch, i2 = method flag kind. */
		txFieldNode* node = (txFieldNode*)self;
		txNode* item = node->item;
		switch (frame->stage) {
		case 0:
			fxCoderAddByte(coder, 1, XS_CODE_THIS);
			if (item->description->token == XS_TOKEN_PROPERTY) {
				frame->i0 = 0;
				frame->stage = 1;
				fxNodeDispatchCode(node->value, coder);
				if (parser->nodeWalkStack != frame)
					return;
				/* fall through */
			}
			else if (item->description->token == XS_TOKEN_PROPERTY_AT) {
				frame->i0 = 1;
				fxCoderAddIndex(coder, 1, XS_CODE_GET_CLOSURE_1, ((txPropertyAtNode*)item)->atAccess->declaration->index);
				frame->stage = 1;
				fxNodeDispatchCode(node->value, coder);
				if (parser->nodeWalkStack != frame)
					return;
				/* fall through */
			}
			else {
				frame->i0 = 2;
				if (item->flags & (mxMethodFlag | mxGetterFlag | mxSetterFlag))
					fxCoderAddIndex(coder, 1, XS_CODE_GET_CLOSURE_1, ((txPrivatePropertyNode*)item)->valueAccess->declaration->index);
				else {
					frame->stage = 1;
					fxNodeDispatchCode(node->value, coder);
					if (parser->nodeWalkStack != frame)
						return;
				}
				/* fall through */
			}
		case 1: {
			txFlag nameFlag = fxNodeCodeName(node->value) ? XS_NAME_FLAG : 0;
			if (frame->i0 == 0)
				fxCoderAddSymbol(coder, -2, XS_CODE_NEW_PROPERTY, ((txPropertyNode*)item)->symbol);
			else if (frame->i0 == 1)
				fxCoderAddByte(coder, -3, XS_CODE_NEW_PROPERTY_AT);
			else
				fxCoderAddIndex(coder, -2, XS_CODE_NEW_PRIVATE_1, ((txPrivatePropertyNode*)item)->symbolAccess->declaration->index);
			if (frame->i0 == 2) {
				if (item->flags & mxMethodFlag)
					fxCoderAddInteger(coder, 0, XS_CODE_INTEGER_1, XS_NAME_FLAG | XS_METHOD_FLAG);
				else if (item->flags & mxGetterFlag)
					fxCoderAddInteger(coder, 0, XS_CODE_INTEGER_1, XS_NAME_FLAG | XS_METHOD_FLAG | XS_GETTER_FLAG);
				else if (item->flags & mxSetterFlag)
					fxCoderAddInteger(coder, 0, XS_CODE_INTEGER_1, XS_NAME_FLAG | XS_METHOD_FLAG | XS_SETTER_FLAG);
				else
					fxCoderAddInteger(coder, 0, XS_CODE_INTEGER_1, nameFlag);
			}
			else
				fxCoderAddInteger(coder, 0, XS_CODE_INTEGER_1, nameFlag);
			fxNodeWalkPopCode(parser);
			return;
		}
		}
		return;
	}
	case WC_CLASS: {
		/* stock: fxClassNodeCode. Two temps (prototype/constructor), scope
		   coding blocks, heritage/constructor dispatches, per-item method
		   and field emission, instanceInit/constructorInit calls. All state
		   lives in the frame: i0=prototype temp, i1=constructor temp,
		   i2=current item flags, v0=former classNode, v1=items cursor,
		   v3=instanceInit, v4=constructorInit, v6=declaration cursor.
		   Each stage runs exactly once: it sets stage=next and returns (the
		   pump re-enters at the next stage; parked children resolve first). */
		txClassNode* node = (txClassNode*)self;
		txNode* item;
		txDeclareNode* declaration;
		txFlag flag;
		switch (frame->stage) {
		case 0:
			frame->v0 = (void*)coder->classNode;
			frame->i0 = fxCoderUseTemporaryVariable(coder);
			frame->i1 = fxCoderUseTemporaryVariable(coder);
			frame->v6 = (void*)node->scope->firstDeclareNode;
			frame->v1 = (void*)node->items->first;
			if (node->symbol)
				fxScopeCodingBlock(node->symbolScope, coder);
			if (node->heritage) {
				frame->stage = 1;
				return;
			}
			fxCoderAddByte(coder, 1, XS_CODE_NULL);
			fxCoderAddByte(coder, 1, XS_CODE_OBJECT);
			frame->stage = 2;
			return;
		case 1:
			if (node->heritage->description->token == XS_TOKEN_HOST)
				fxCoderAddByte(coder, 1, XS_CODE_NULL);
			frame->stage = 2;
			fxNodeDispatchCode(node->heritage, coder);
			if (parser->nodeWalkStack != frame)
				return;
			return;
		case 2:
			/* The heritage child may have parked stage 1. Emit EXTEND on
			   its continuation, once the superclass expression is ready. */
			if (node->heritage && node->heritage->description->token != XS_TOKEN_HOST)
				fxCoderAddByte(coder, 1, XS_CODE_EXTEND);
			fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, frame->i0);
			fxScopeCodingBlock(node->scope, coder);
			coder->classNode = node;
			frame->stage = 3;
			fxNodeDispatchCode(node->constructor, coder);
			return;
		case 3:
			fxCoderAddByte(coder, 0, XS_CODE_TO_INSTANCE);
			fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, frame->i1);
			fxCoderAddByte(coder, -3, XS_CODE_CLASS);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, frame->i1);
			if (node->symbol)
				fxCoderAddSymbol(coder, 0, XS_CODE_NAME, node->symbol);
			if (!frame->v1)
				frame->stage = 6;
			else
				frame->stage = 4;
			return;
		case 4:
			/* item loop head: route by kind; non-emitting items just skip.
			   Re-entered after every skip, so the cursor must be re-checked
			   for end-of-list before dereferencing (stock: while (item)). */
			if (!frame->v1) {
				frame->stage = 6;
				return;
			}
			item = (txNode*)frame->v1;
			frame->i2 = item->flags;
			if (item->description->token == XS_TOKEN_PROPERTY) {
				if (item->flags & (mxMethodFlag | mxGetterFlag | mxSetterFlag)) {
					frame->stage = 10;
					return;
				}
				frame->v1 = item->next;
				return;
			}
			else if (item->description->token == XS_TOKEN_PROPERTY_AT) {
				if (item->flags & (mxMethodFlag | mxGetterFlag | mxSetterFlag)) {
					frame->stage = 11;
					return;
				}
				frame->stage = 13;
				return;
			}
			frame->stage = 15;
			return;
		case 10:
			/* PROPERTY method: receiver, value dispatch, NEW_PROPERTY */
			item = (txNode*)frame->v1;
			if (item->flags & mxStaticFlag)
				fxCoderAddByte(coder, 1, XS_CODE_DUB);
			else
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, frame->i0);
			frame->stage = 12;
			fxNodeDispatchCode(((txPropertyNode*)item)->value, coder);
			return;
		case 11:
			/* PROPERTY_AT method: receiver, at dispatch */
			item = (txNode*)frame->v1;
			if (item->flags & mxStaticFlag)
				fxCoderAddByte(coder, 1, XS_CODE_DUB);
			else
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, frame->i0);
			frame->stage = 14;
			fxNodeDispatchCode(((txPropertyAtNode*)item)->at, coder);
			return;
		case 12:
			item = (txNode*)frame->v1;
			if (item->description->token == XS_TOKEN_PROPERTY_AT)
				fxCoderAddByte(coder, -3, XS_CODE_NEW_PROPERTY_AT);
			else
				fxCoderAddSymbol(coder, -2, XS_CODE_NEW_PROPERTY, ((txPropertyNode*)item)->symbol);
			flag = XS_DONT_ENUM_FLAG;
			if (frame->i2 & mxMethodFlag)
				flag |= XS_NAME_FLAG | XS_METHOD_FLAG;
			else if (frame->i2 & mxGetterFlag)
				flag |= XS_NAME_FLAG | XS_METHOD_FLAG | XS_GETTER_FLAG;
			else if (frame->i2 & mxSetterFlag)
				flag |= XS_NAME_FLAG | XS_METHOD_FLAG | XS_SETTER_FLAG;
			fxCoderAddInteger(coder, 0, XS_CODE_INTEGER_1, flag);
			frame->v1 = item->next;
			frame->stage = 4;
			return;
		case 13:
			/* PROPERTY_AT field: at dispatch (no receiver in stock) */
			item = (txNode*)frame->v1;
			frame->stage = 14;
			fxNodeDispatchCode(((txPropertyAtNode*)item)->at, coder);
			return;
		case 14:
			item = (txNode*)frame->v1;
			fxCoderAddByte(coder, 0, XS_CODE_AT);
			if (frame->i2 & (mxMethodFlag | mxGetterFlag | mxSetterFlag)) {
				frame->stage = 12;
				fxNodeDispatchCode(((txPropertyAtNode*)item)->value, coder);
				return;
			}
			declaration = (txDeclareNode*)frame->v6;
			fxCoderAddIndex(coder, 0, XS_CODE_CONST_CLOSURE_1, declaration->index);
			frame->v6 = (void*)declaration->nextDeclareNode;
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			frame->v1 = item->next;
			frame->stage = 4;
			return;
		case 15:
			/* PRIVATE: leading CONST_CLOSURE always; methods dispatch value */
			item = (txNode*)frame->v1;
			declaration = (txDeclareNode*)frame->v6;
			fxCoderAddIndex(coder, 0, XS_CODE_CONST_CLOSURE_1, declaration->index);
			frame->v6 = (void*)declaration->nextDeclareNode;
			if (item->flags & (mxMethodFlag | mxGetterFlag | mxSetterFlag)) {
				frame->stage = 17;
				fxNodeDispatchCode(((txPrivatePropertyNode*)item)->value, coder);
				return;
			}
			frame->v1 = item->next;
			frame->stage = 4;
			return;
		case 17:
			declaration = (txDeclareNode*)frame->v6;
			fxCoderAddIndex(coder, 0, XS_CODE_CONST_CLOSURE_1, declaration->index);
			frame->v6 = (void*)declaration->nextDeclareNode;
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			item = (txNode*)frame->v1;
			frame->v1 = item->next;
			frame->stage = 4;
			return;
		case 6:
			/* items done: symbol closure, then instanceInit/constructorInit */
			if (node->symbol)
				fxCoderAddIndex(coder, 0, XS_CODE_CONST_CLOSURE_1, node->symbolScope->firstDeclareNode->index);
			if (node->instanceInit) {
				frame->v3 = node->instanceInit;
				frame->stage = 60;
				return;
			}
			if (node->constructorInit) {
				frame->v4 = node->constructorInit;
				frame->stage = 61;
				return;
			}
			frame->stage = 63;
			return;
		case 60:
			/* instanceInit: dispatch, SET_HOME onto prototype, closure, POP */
			frame->stage = 62;
			fxNodeDispatchCode((txNode*)frame->v3, coder);
			return;
		case 62:
			declaration = (txDeclareNode*)frame->v6;
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, frame->i0);
			fxCoderAddByte(coder, -1, XS_CODE_SET_HOME);
			fxCoderAddIndex(coder, 0, XS_CODE_CONST_CLOSURE_1, declaration->index);
			frame->v6 = (void*)declaration->nextDeclareNode;
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			if (node->constructorInit) {
				frame->v4 = node->constructorInit;
				frame->stage = 61;
				return;
			}
			frame->stage = 63;
			return;
		case 61:
			/* constructorInit: GET_LOCAL constructor, dispatch, GET_LOCAL
			   again, SET_HOME, CALL (stock emits the GET_LOCAL_1 both
			   before and after the dispatch). */
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, frame->i1);
			frame->stage = 64;
			fxNodeDispatchCode((txNode*)frame->v4, coder);
			return;
		case 64:
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, frame->i1);
			fxCoderAddByte(coder, -1, XS_CODE_SET_HOME);
			fxCoderAddByte(coder, 1, XS_CODE_CALL);
			fxCoderAddInteger(coder, -2, XS_CODE_RUN_1, 0);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			frame->stage = 63;
			return;
		case 63:
			coder->classNode = (txClassNode*)frame->v0;
			fxScopeCoded(node->scope, coder);
			if (node->symbol)
				fxScopeCoded(node->symbolScope, coder);
			fxCoderUnuseTemporaryVariables(coder, 2);
			fxNodeWalkPopCode(parser);
			return;
		}
		return;
	}
	case WC_FUNCTION: {
		txFunctionNode* node = (txFunctionNode*)self;
		txSymbol* name = node->symbol;
		txTargetCode* target;
		switch (frame->stage) {
		case 0:
			/* save environment */
			frame->i0 = coder->environmentLevel;
			frame->i1 = coder->evalFlag;
			frame->i2 = coder->line;
			frame->i3 = coder->programFlag;
			frame->i4 = coder->scopeLevel;
			frame->v4 = coder->firstBreakTarget;
			frame->v5 = coder->firstContinueTarget;
			frame->v6 = coder->returnTarget;
			frame->v1 = (void*)name;
			target = fxCoderCreateTarget(coder);
			frame->v0 = target;
			if ((node->flags & mxEvalFlag) && !(node->flags & mxStrictFlag))
				coder->evalFlag = 1;
			coder->line = kNoLine;
			coder->programFlag = 0;
			coder->scopeLevel = 0;
			coder->firstBreakTarget = NULL;
			coder->firstContinueTarget = NULL;
			if (name) {
				if (node->flags & mxGetterFlag) {
					txString buffer = coder->parser->buffer;
					c_strcpy(buffer, "get ");
					c_strcat(buffer, name->string);
					name = fxNewParserSymbol(coder->parser, buffer);
				}
				else if (node->flags & mxSetterFlag) {
					txString buffer = coder->parser->buffer;
					c_strcpy(buffer, "set ");
					c_strcat(buffer, name->string);
					name = fxNewParserSymbol(coder->parser, buffer);
				}
			}
			frame->v1 = (void*)name;
			if (node->flags & mxAsyncFlag) {
				if (node->flags & mxGeneratorFlag)
					fxCoderAddSymbol(coder, 1, XS_CODE_ASYNC_GENERATOR_FUNCTION, name);
				else
					fxCoderAddSymbol(coder, 1, XS_CODE_ASYNC_FUNCTION, name);
			}
			else if (node->flags & mxGeneratorFlag)
				fxCoderAddSymbol(coder, 1, XS_CODE_GENERATOR_FUNCTION, name);
			else if (node->flags & (mxArrowFlag | mxMethodFlag | mxGetterFlag | mxSetterFlag))
				fxCoderAddSymbol(coder, 1, XS_CODE_FUNCTION, name);
			else
				fxCoderAddSymbol(coder, 1, XS_CODE_CONSTRUCTOR_FUNCTION, name);
			if (coder->parser->flags & mxDebugFlag)
				fxCoderAddByte(coder, 0, XS_CODE_PROFILE);
			fxCoderAddBranch(coder, 0, XS_CODE_CODE_1, target);
			if (node->flags & mxFieldFlag)
				fxCoderAddIndex(coder, 0, XS_CODE_BEGIN_STRICT_FIELD, fxCoderCountParameters(coder, node->params));
			else if (node->flags & mxDerivedFlag)
				fxCoderAddIndex(coder, 0, XS_CODE_BEGIN_STRICT_DERIVED, fxCoderCountParameters(coder, node->params));
			else if (node->flags & mxBaseFlag)
				fxCoderAddIndex(coder, 0, XS_CODE_BEGIN_STRICT_BASE, fxCoderCountParameters(coder, node->params));
			else if (node->flags & mxStrictFlag)
				fxCoderAddIndex(coder, 0, XS_CODE_BEGIN_STRICT, fxCoderCountParameters(coder, node->params));
			else
				fxCoderAddIndex(coder, 0, XS_CODE_BEGIN_SLOPPY, fxCoderCountParameters(coder, node->params));
			coder->path = C_NULL;
			if (node->line != kNoLine)
				fxCoderAddLine(coder, 0, XS_CODE_LINE, self);
			if (node->scopeCount)
				fxCoderAddIndex(coder, 0, XS_CODE_RESERVE_1, node->scopeCount);
			fxScopeCodeRetrieve(node->scope, coder);
			fxScopeCodingParams(node->scope, coder);
			if ((node->flags & mxAsyncFlag) && !(node->flags & mxGeneratorFlag))
				fxCoderAddByte(coder, 0, XS_CODE_START_ASYNC);
			if (node->flags & mxBaseFlag) {
				if (coder->classNode->instanceInitAccess) {
					fxCoderAddByte(coder, 1, XS_CODE_THIS);
					fxCoderAddIndex(coder, 1, XS_CODE_GET_CLOSURE_1, coder->classNode->instanceInitAccess->declaration->index);
					fxCoderAddByte(coder, 1, XS_CODE_CALL);
					fxCoderAddInteger(coder, -2, XS_CODE_RUN_1, 0);
					fxCoderAddByte(coder, -1, XS_CODE_POP);
				}
			}
			frame->stage = 1;
			fxNodeDispatchCode(node->params, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 1:
			if ((coder->parser->flags & mxDebugFlag) && coder->path)
				fxCoderAddIndex(coder, 0, XS_CODE_LINE, 0);
			/* xs_no_recursion (R7/B5c): see running-clear note at WC_BLOCK. */
			{
				int running_nr = parser->nodeWalkRunning;
				parser->nodeWalkRunning = 0;
				fxScopeCodeDefineNodes(node->scope, coder);
				parser->nodeWalkRunning = running_nr;
			}
			frame->v2 = fxCoderCreateTarget(coder);
			coder->returnTarget = (txTargetCode*)frame->v2;
			if (node->flags & mxGeneratorFlag) {
				if (node->flags & mxAsyncFlag)
					fxCoderAddByte(coder, 0, XS_CODE_START_ASYNC_GENERATOR);
				else
					fxCoderAddByte(coder, 0, XS_CODE_START_GENERATOR);
			}
			frame->stage = 2;
			fxNodeDispatchCode(node->body, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 2:
			fxCoderAdd(coder, 0, (txTargetCode*)frame->v2);
			if (node->flags & mxArrowFlag)
				fxCoderAddByte(coder, 0, XS_CODE_END_ARROW);
			else if (node->flags & mxBaseFlag)
				fxCoderAddByte(coder, 0, XS_CODE_END_BASE);
			else if (node->flags & mxDerivedFlag)
				fxCoderAddByte(coder, 0, XS_CODE_END_DERIVED);
			else
				fxCoderAddByte(coder, 0, XS_CODE_END);
			fxCoderAdd(coder, 0, (txTargetCode*)frame->v0);
			if ((node->scope->flags & mxEvalFlag) || coder->evalFlag) {
				fxCoderAddByte(coder, 1, XS_CODE_FUNCTION_ENVIRONMENT);
				fxScopeCodeStore(node->scope, coder);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
			}
			else if (node->scope->closureNodeCount || ((node->flags & mxArrowFlag) && (node->flags & mxDefaultFlag))) {
				fxCoderAddByte(coder, 1, XS_CODE_ENVIRONMENT);
				fxScopeCodeStore(node->scope, coder);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
			}
			if ((node->flags & (mxArrowFlag | mxBaseFlag | mxDerivedFlag | mxGeneratorFlag | mxStrictFlag | mxMethodFlag)) == 0) {
				fxCoderAddByte(coder, 1, XS_CODE_DUB);
				fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
				fxCoderAddSymbol(coder, -2, XS_CODE_NEW_PROPERTY, coder->parser->callerSymbol);
				fxCoderAddInteger(coder, 0, XS_CODE_INTEGER_1, XS_DONT_ENUM_FLAG);
			}
			/* restore environment */
			coder->returnTarget = (txTargetCode*)frame->v6;
			coder->firstContinueTarget = (txTargetCode*)frame->v5;
			coder->firstBreakTarget = (txTargetCode*)frame->v4;
			coder->scopeLevel = (txInteger)frame->i4;
			coder->programFlag = (txBoolean)frame->i3;
			coder->line = (txInteger)frame->i2;
			coder->evalFlag = (txBoolean)frame->i1;
			coder->environmentLevel = (txInteger)frame->i0;
			fxNodeWalkPopCode(parser);
			return;
		}
		return;
	}
	case WC_TEMPLATE:
		/* xs_no_recursion (R7/B3): stock fxTemplateNodeCode. Tagged path:
		   prologue (with This dispatch), fill loop (string/raw dispatches
		   per MIDDLE item, phase-tracked in i3), epilogue, tail loop
		   (expression dispatches). Untagged path: string/item dispatches
		   with TO_STRING/ADD between. i0 = i, i1 = strings temp,
		   i2 = raws temp / untagged was-expression flag, i3 = fill phase
		   or tail arg count; v0 = item cursor, v1 = tag symbol,
		   v2 = cacheTarget. stages: 0/1 prologue, 1 fill, 2 tail loop,
		   3 tail resume, 4/5 untagged, 6 untagged resume. */
		switch (frame->stage) {
		case 0: {
				txTemplateNode* node = (txTemplateNode*)self;
				if (!node->reference) {
					frame->v0 = node->items->first;
					frame->i3 = 0;
					frame->stage = 4;
					goto wc_template_untagged;
				}
				{
					txSymbol* symbol;
					frame->i0 = (node->items->length / 2) + 1;
					frame->i2 = fxCoderUseTemporaryVariable(coder);
					frame->i1 = fxCoderUseTemporaryVariable(coder);
					frame->stage = 1;
					fxNodeDispatchCodeThis(node->reference, coder, 0);
					if (parser->nodeWalkStack != frame)
						return;
				}
				/* fall through */
			}
		case 1: {
				txTemplateNode* node = (txTemplateNode*)self;
				txSymbol* symbol;
				fxCoderAddByte(coder, 1, XS_CODE_CALL);
				fxGenerateTag(parser->console, parser->buffer, parser->bufferSize, (parser->path) ? parser->path->string : C_NULL);
				symbol = fxNewParserSymbol(parser, parser->buffer);
				frame->v1 = symbol;
				fxCoderAddByte(coder, 1, XS_CODE_TEMPLATE_CACHE);
				fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, symbol);
				frame->v2 = fxCoderCreateTarget(coder);
				fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_COALESCE_1, (txTargetCode*)frame->v2);
				fxCoderAddByte(coder, 1, XS_CODE_TEMPLATE_CACHE);
				fxCoderAddByte(coder, 1, XS_CODE_ARRAY);
				fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->i1);
				fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, (txInteger)frame->i0);
				fxCoderAddSymbol(coder, -1, XS_CODE_SET_PROPERTY, coder->parser->lengthSymbol);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
				fxCoderAddByte(coder, 1, XS_CODE_ARRAY);
				fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->i2);
				fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, (txInteger)frame->i0);
				fxCoderAddSymbol(coder, -1, XS_CODE_SET_PROPERTY, coder->parser->lengthSymbol);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
				frame->i0 = 0;
				frame->v0 = node->items->first;
				frame->i3 = 0;
				frame->stage = 2;
				goto wc_template_fill;
			}
		case 2:
		wc_template_fill:
			for (;;) {
				txNode* item = (txNode*)frame->v0;
				if (!item) {
					/* fill epilogue: no child dispatches left */
					txInteger flag = XS_DONT_DELETE_FLAG | XS_DONT_SET_FLAG;
					(void)flag;
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i1);
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i2);
					fxCoderAddSymbol(coder, -1, XS_CODE_SET_PROPERTY, parser->rawSymbol);
					fxCoderAddByte(coder, -1, XS_CODE_POP);
					fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i1);
					fxCoderAddByte(coder, 0, XS_CODE_TEMPLATE);
					fxCoderAddSymbol(coder, -1, XS_CODE_SET_PROPERTY, (txSymbol*)frame->v1);
					fxCoderAdd(coder, 0, (txTargetCode*)frame->v2);
					frame->i3 = 1;		/* run arg count: tag + expressions */
					frame->v0 = ((txTemplateNode*)self)->items->first;
					frame->stage = 3;
					goto wc_template_tail;
				}
				if (item->description->token == XS_TOKEN_TEMPLATE_MIDDLE) {
					txInteger flag = XS_DONT_DELETE_FLAG | XS_DONT_SET_FLAG;
					if (frame->i3 == 0) {
						fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i1);
						fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, (txInteger)frame->i0);
						fxCoderAddByte(coder, 0, XS_CODE_AT);
						frame->i3 = 1;
						if (((txTemplateItemNode*)item)->string->flags & mxStringErrorFlag)
							fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
						else {
							fxNodeDispatchCode(((txTemplateItemNode*)item)->string, coder);
							if (parser->nodeWalkStack != frame)
								return;
						}
					}
					if (frame->i3 == 1) {
						fxCoderAddByte(coder, -3, XS_CODE_NEW_PROPERTY_AT);
						fxCoderAddInteger(coder, 0, XS_CODE_INTEGER_1, flag);
						fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i2);
						fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, (txInteger)frame->i0);
						fxCoderAddByte(coder, 0, XS_CODE_AT);
						frame->i3 = 2;
						fxNodeDispatchCode(((txTemplateItemNode*)item)->raw, coder);
						if (parser->nodeWalkStack != frame)
							return;
					}
					fxCoderAddByte(coder, -3, XS_CODE_NEW_PROPERTY_AT);
					fxCoderAddInteger(coder, 0, XS_CODE_INTEGER_1, flag);
					frame->i0 = (txInteger)frame->i0 + 1;
					frame->i3 = 0;
					frame->v0 = item->next;
					continue;
				}
				/* non-MIDDLE item: not part of the fill loop, just advance */
				frame->v0 = item->next;
			}
		case 3:
		wc_template_tail:
			for (;;) {
				txNode* item = (txNode*)frame->v0;
				if (!item) {
					fxCoderAddInteger(coder, -2 - (txInteger)frame->i3, (self->flags & mxTailRecursionFlag) ? XS_CODE_RUN_TAIL_1 : XS_CODE_RUN_1, (txInteger)frame->i3);
					fxCoderUnuseTemporaryVariables(coder, 2);
					fxNodeWalkPopCode(parser);
					return;
				}
				if (item->description->token != XS_TOKEN_TEMPLATE_MIDDLE) {
					/* park ON the item: the resume (stage 6) advances past it */
					frame->stage = 6;
					fxNodeDispatchCode(item, coder);
					if (parser->nodeWalkStack != frame)
						return;
					frame->i3 = (txInteger)frame->i3 + 1;
					frame->v0 = item->next;
					continue;
				}
				frame->v0 = item->next;
			}
		case 4:
		wc_template_untagged:
			{
				txNode* item = (txNode*)frame->v0;
				if (!item) {
					fxNodeWalkPopCode(parser);
					return;
				}
				if (frame->i3 == 0) {
					/* stock dispatches the first chunk's string with no
					   TO_STRING and no ADD before it */
					frame->v1 = ((txTemplateItemNode*)item)->string;
					frame->i2 = 0;
					frame->i1 = 0;	/* not preceded by an item: no ADD */
				}
				else if (item->description->token == XS_TOKEN_TEMPLATE_MIDDLE) {
					frame->v1 = ((txTemplateItemNode*)item)->string;
					frame->i2 = 0;
					frame->i1 = 1;
				}
				else {
					frame->v1 = item;
					frame->i2 = 1;
					frame->i1 = 1;
				}
				frame->i3 = 1;	/* a previous item now exists */
				frame->v0 = item->next;
				frame->stage = 5;
				fxNodeDispatchCode((txNode*)frame->v1, coder);
				if (parser->nodeWalkStack != frame)
					return;
				/* fall through */
			}
		case 5:
			if (frame->i2)
				fxCoderAddByte(coder, 1, XS_CODE_TO_STRING);
			if (frame->i1)
				fxCoderAddByte(coder, -1, XS_CODE_ADD);
			frame->stage = 4;
			goto wc_template_untagged;
		case 6:
			/* tail-loop expression child completed: bump arg count and
			   advance past the parked item */
			frame->i3 = (txInteger)frame->i3 + 1;
			frame->v0 = ((txNode*)frame->v0)->next;
			frame->stage = 3;
			goto wc_template_tail;
		}
		return;
		/* stock: fxSpreadNodeCode — full iterator protocol over
		   self->expression, bumping the counter local. */
		switch (frame->stage) {
		case 0: {
				txSpreadNode* node = (txSpreadNode*)self;
				frame->v0 = fxCoderCreateTarget(coder);
				frame->v1 = fxCoderCreateTarget(coder);
				frame->stage = 1;
				fxNodeDispatchCode(node->expression, coder);
				if (parser->nodeWalkStack != frame)
					return;
				/* fall through */
			case 1:
				fxCoderAddByte(coder, 0, XS_CODE_FOR_OF);
				frame->i0 = fxCoderUseTemporaryVariable(coder);
				fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->i0);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
				fxCoderAdd(coder, 0, (txTargetCode*)frame->v0);
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->i0);
				fxCoderAddByte(coder, 1, XS_CODE_DUB);
				fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->nextSymbol);
				fxCoderAddByte(coder, 1, XS_CODE_CALL);
				fxCoderAddInteger(coder, -2, XS_CODE_RUN_1, 0);
				fxCoderAddByte(coder, 1, XS_CODE_DUB);
				fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->doneSymbol);
				fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, (txTargetCode*)frame->v1);
				fxCoderAddSymbol(coder, 0, XS_CODE_GET_PROPERTY, coder->parser->valueSymbol);
				fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, (txInteger)frame->param);
				fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, 1);
				fxCoderAddByte(coder, -1, XS_CODE_ADD);
				fxCoderAddIndex(coder, 0, XS_CODE_SET_LOCAL_1, (txInteger)frame->param);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
				fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, (txTargetCode*)frame->v0);
				fxCoderAdd(coder, 1, (txTargetCode*)frame->v1);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
				fxCoderUnuseTemporaryVariables(coder, 1);
				fxNodeWalkPopCode(parser);
			}
			return;
		}
	/* R7/B5c: stock fxImportCallNodeCode. Two child dispatches (expression,
	   optional withExpression), then importFlag=1 and IMPORT. */
	case WC_IMPORT_CALL:
		if (frame->stage == 0) {
			frame->stage = 1;
			fxNodeDispatchCode(((txImportCallNode*)self)->expression, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		}
		if (frame->stage == 1) {
			if (((txImportCallNode*)self)->withExpression) {
				frame->stage = 2;
				fxNodeDispatchCode(((txImportCallNode*)self)->withExpression, coder);
				if (parser->nodeWalkStack != frame)
					return;
				/* fall through */
			}
			else {
				fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
			}
			/* fall through */
		}
		coder->importFlag = 1;
		fxCoderAddByte(coder, -1, XS_CODE_IMPORT);
		fxNodeWalkPopCode(parser);
		return;
	case WC_INCLUDE:
		/* R7/B5c: stock fxIncludeNodeCode — one child dispatch. */
		if (frame->stage == 0) {
			frame->stage = 1;
			fxNodeDispatchCode(((txIncludeNode*)self)->body, coder);
			if (parser->nodeWalkStack != frame)
				return;
		}
		fxNodeWalkPopCode(parser);
		return;
	case WC_MODULE: {
		/* R7/B5c: stock fxModuleNodeCode. v4 = current-half target
		   (POINTER: v-slot, not i-slot — mx32bitID truncates),
		   i1 = 1 while the first (prelude) half is in progress,
		   i2 = debug flag, i3 = scopeCount flag, v0 = declaration cursor,
		   v0..v3 = using context parked across the body dispatch (exception,
		   selector, catchTarget, disposableNodeCount).
		   stock order: reset coder fields, count DEFINE|VAR declarations,
		   [count: FUNCTION/PROFILE/CODE_1/BEGIN_STRICT/LINE/RESERVE_1/
		   Retrieve/VAR loop/DefineNodes/END/target/ENVIRONMENT/POP + reset]
		   else NULL; then FUNCTION|ASYNC_FUNCTION/PROFILE/CODE_1/
		   BEGIN_STRICT/LINE/RESERVE_1/Retrieve/[START_ASYNC]/returnTarget/
		   [Using]/body/[Used]/returnTarget/END/target/ENVIRONMENT/POP/
		   INTEGER_1 count/MODULE flag/SET_RESULT/END. */
		txModuleNode* node = (txModuleNode*)self;
		txDeclareNode* declaration;
		txInteger count;
		txFlag flag = 0;
		switch (frame->stage) {
		case 0:
			/* xs_no_recursion (R7/B5c): targets are POINTERS — keep them in
			   v-slots (this build compiles with mx32bitID=1, txInteger is
			   32-bit; an i-slot truncates the pointer). */
			frame->v4 = (void*)fxCoderCreateTarget(coder);
			frame->i1 = 1;
			frame->i2 = (coder->parser->flags & mxDebugFlag) ? 1 : 0;
			frame->i3 = node->scopeCount ? 1 : 0;
			coder->line = kNoLine;
			coder->programFlag = 0;
			coder->scopeLevel = 0;
			coder->firstBreakTarget = NULL;
			coder->firstContinueTarget = NULL;
			count = 0;
			declaration = node->scope->firstDeclareNode;
			while (declaration) {
				if ((declaration->description->token == XS_TOKEN_DEFINE) || (declaration->description->token == XS_TOKEN_VAR))
					count++;
				declaration = declaration->nextDeclareNode;
			}
			if (!count) {
				fxCoderAddByte(coder, 1, XS_CODE_NULL);
				frame->stage = 2;
				return;
			}
			fxCoderAddSymbol(coder, 1, XS_CODE_FUNCTION, C_NULL);
			if (frame->i2)
				fxCoderAddByte(coder, 0, XS_CODE_PROFILE);
			fxCoderAddBranch(coder, 0, XS_CODE_CODE_1, (txTargetCode*)frame->v4);
			fxCoderAddIndex(coder, 0, XS_CODE_BEGIN_STRICT, 0);
			coder->path = C_NULL;
			if (node->line != kNoLine)
				fxCoderAddLine(coder, 0, XS_CODE_LINE, self);
			if (frame->i3)
				fxCoderAddIndex(coder, 0, XS_CODE_RESERVE_1, node->scopeCount);
			fxScopeCodeRetrieve(node->scope, coder);
			frame->v0 = (void*)node->scope->firstDeclareNode;
			frame->stage = 1;
			return;
		case 1:
			/* VAR loop: UNDEFINED / VAR_CLOSURE_1 / POP per VAR declaration. */
			declaration = (txDeclareNode*)frame->v0;
			while (declaration) {
				if (declaration->description->token == XS_TOKEN_VAR) {
					fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
					fxCoderAddIndex(coder, 0, XS_CODE_VAR_CLOSURE_1, declaration->index);
					fxCoderAddByte(coder, -1, XS_CODE_POP);
				}
				declaration = declaration->nextDeclareNode;
			}
			/* xs_no_recursion (R7/B5c): see running-clear note at WC_BLOCK. */
			{
				int running_nr = parser->nodeWalkRunning;
				parser->nodeWalkRunning = 0;
				fxScopeCodeDefineNodes(node->scope, coder);
				parser->nodeWalkRunning = running_nr;
			}
			fxCoderAddByte(coder, 0, XS_CODE_END);
			fxCoderAdd(coder, 0, (txTargetCode*)frame->v4);
			fxCoderAddByte(coder, 1, XS_CODE_ENVIRONMENT);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			frame->v4 = (void*)fxCoderCreateTarget(coder);
			frame->i1 = 0;
			coder->line = kNoLine;
			coder->programFlag = 0;
			coder->scopeLevel = 0;
			coder->firstBreakTarget = NULL;
			coder->firstContinueTarget = NULL;
			/* fall through */
		case 2:
			if (node->flags & mxAwaitingFlag)
				fxCoderAddSymbol(coder, 1, XS_CODE_ASYNC_FUNCTION, C_NULL);
			else
				fxCoderAddSymbol(coder, 1, XS_CODE_FUNCTION, C_NULL);
			if (frame->i2)
				fxCoderAddByte(coder, 0, XS_CODE_PROFILE);
			fxCoderAddBranch(coder, 0, XS_CODE_CODE_1, (txTargetCode*)frame->v4);
			fxCoderAddIndex(coder, 0, XS_CODE_BEGIN_STRICT, 0);
			coder->path = C_NULL;
			if (node->line != kNoLine)
				fxCoderAddLine(coder, 0, XS_CODE_LINE, self);
			if (frame->i3)
				fxCoderAddIndex(coder, 0, XS_CODE_RESERVE_1, node->scopeCount);
			fxScopeCodeRetrieve(node->scope, coder);
			if (node->flags & mxAwaitingFlag)
				fxCoderAddByte(coder, 0, XS_CODE_START_ASYNC);
			coder->returnTarget = fxCoderCreateTarget(coder);
			frame->v0 = C_NULL;
			frame->v1 = C_NULL;
			if (node->scope->disposableNodeCount) {
				txUsingContext context;
				fxScopeCodeUsing(node->scope, coder, &context);
				frame->v0 = (void*)(txInteger)context.exception;
				frame->v1 = (void*)(txInteger)context.selector;
				frame->v2 = (void*)context.catchTarget;
				frame->v3 = (void*)(txInteger)node->scope->disposableNodeCount;
			}
			frame->stage = 3;
			fxNodeDispatchCode(node->body, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 3:
			if (frame->v3) {
				txUsingContext context;
				context.exception = (txInteger)(intptr_t)frame->v0;
				context.selector = (txInteger)(intptr_t)frame->v1;
				context.catchTarget = (txTargetCode*)frame->v2;
				fxScopeCodeUsed(node->scope, coder, &context);
			}
			fxCoderAdd(coder, 0, coder->returnTarget);
			fxCoderAddByte(coder, 0, XS_CODE_END);
			fxCoderAdd(coder, 0, (txTargetCode*)frame->v4);
			fxCoderAddByte(coder, 1, XS_CODE_ENVIRONMENT);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			count = 2 + fxScopeCodeSpecifierNodes(node->scope, coder);
			fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, count);
			if (!(node->flags & mxStrictFlag))
				flag |= XS_JSON_MODULE_FLAG;
			if (coder->importFlag)
				flag |= XS_IMPORT_FLAG;
			if (coder->importMetaFlag)
				flag |= XS_IMPORT_META_FLAG;
			fxCoderAddIndex(coder, 0 - count, XS_CODE_MODULE, flag);
			fxCoderAddByte(coder, -1, XS_CODE_SET_RESULT);
			fxCoderAddByte(coder, 0, XS_CODE_END);
			fxNodeWalkPopCode(parser);
			return;
		}
		return;
	}
	case WC_PROGRAM:
		/* R7/B5c: stock fxProgramNodeCode. Root entry bypasses
		   fxNodeDispatchCode, so the native emitter converts itself into a
		   frame below. i0 = returnTarget parked across the body dispatch. */
		switch (frame->stage) {
		case 0:
			coder->line = kNoLine;
			coder->programFlag = 1;
			coder->scopeLevel = 0;
			coder->firstBreakTarget = NULL;
			coder->firstContinueTarget = NULL;
			if (((txProgramNode*)self)->flags & mxStrictFlag)
				fxCoderAddIndex(coder, 0, XS_CODE_BEGIN_STRICT, 0);
			else
				fxCoderAddIndex(coder, 0, XS_CODE_BEGIN_SLOPPY, 0);
			coder->path = C_NULL;
			if (self->line != kNoLine)
				fxCoderAddLine(coder, 0, XS_CODE_LINE, self);
			if (coder->parser->flags & mxEvalFlag) {
				coder->evalFlag = 1;
				fxScopeCodingEval(((txProgramNode*)self)->scope, coder);
			}
			else
				fxScopeCodingProgram(((txProgramNode*)self)->scope, coder);
			coder->returnTarget = fxCoderCreateTarget(coder);
			/* xs_no_recursion (R7/B5c): see running-clear note at WC_BLOCK. */
			{
				int running_nr = parser->nodeWalkRunning;
				parser->nodeWalkRunning = 0;
				fxScopeCodeDefineNodes(((txProgramNode*)self)->scope, coder);
				parser->nodeWalkRunning = running_nr;
			}
			frame->stage = 1;
			fxNodeDispatchCode(((txProgramNode*)self)->body, coder);
			if (parser->nodeWalkStack != frame)
				return;
			/* fall through */
		case 1:
			fxCoderAdd(coder, 0, coder->returnTarget);
			fxCoderAddByte(coder, 0, XS_CODE_RETURN);
			fxNodeWalkPopCode(parser);
			return;
		}
		return;
	case WC_NATIVE:
		/* xs_no_recursion (R7/B4): pop self BEFORE running the stock
		   emitter. The emitter runs with running=0 so its native
		   descendants dispatch natively; any converted descendant then
		   creates a nested Call wrapper whose drain stops at the stack
		   level below this frame — re-entering this frame would loop
		   forever, so it must already be gone. */
		fxNodeWalkPopCode(parser);
		fxNodeWalkSetRunningCode(parser, 0);
		(*self->description->dispatch->code)(self, coder);
		return;
	case WC_NASSIGN:
		/* xs_no_recursion (R7/B2): stage carries the flag. Same
		   pop-before-run discipline as WC_NATIVE. */
		fxNodeWalkPopCode(parser);
		fxNodeWalkSetRunningCode(parser, 0);
		(*self->description->dispatch->codeAssign)(self, coder, (txFlag)frame->stage);
		return;
	case WC_NREF:
		fxNodeWalkPopCode(parser);
		fxNodeWalkSetRunningCode(parser, 0);
		(*self->description->dispatch->codeReference)(self, coder, (txFlag)frame->stage);
		return;
	case WC_NTHIS:
		fxNodeWalkPopCode(parser);
		fxNodeWalkSetRunningCode(parser, 0);
		parser->codeThisResult = (*self->description->dispatch->codeThis)(self, coder, (txFlag)frame->stage);
		return;
	default:
		/* xs_no_recursion (R7/B5a): a converted kind without a machine must
		   not fall through into a neighboring case; report loudly. */
		fxReportParserError(parser, self ? self->line : 0, "xs_no_recursion: unhandled pump kind %d", frame->kind);
		fxNodeWalkPopCode(parser);
		return;
	}
}

static txFlag fxNodeCodeName(txNode* value);
static void fxCompoundExpressionNodeCodeName(void* it, void* param);
static void fxSpreadNodeCode(void* it, void* param, txInteger counter);

txScript* fxParserCode(txParser* parser)
{
	txCoder coder;
	txByteCode* code;
	txScript* script;
	txSize size, delta, offset;
	txSymbol* symbol;
	txSymbol** address;
	txSize c, i;
	txID id, count;
	txSize total;
	txByte* p;
	txHostNode* node;
	/* R26f: exempt the coder from the parse-only parser cap
	 * (fxNRParserTotalCap) for this whole function — codegen allocates every
	 * bytecode node through the same chunk funnel the parse cap bounds.
	 * Restored on every exit path below. */
	int savedCodegen = fxNRParserCodegen;
	fxNRParserCodegen = 1;
#ifdef mxMetering
	txUnsigned meterIndex = 0;
#endif

    c_memset(&coder, 0, sizeof(txCoder));
	coder.parser = parser;
	if (parser->errorCount == 0) {
		mxTryParser(parser) {
			txNode* self = parser->root;
			(*self->description->dispatch->code)(parser->root, &coder);
		}
		mxCatchParser(parser) {
		}
	}
	if (parser->errorCount) {
		if (parser->console) {
			coder.firstCode = NULL;
			coder.lastCode = NULL;
			fxCoderAddByte(&coder, 1, XS_CODE_GLOBAL);
			fxCoderAddSymbol(&coder, 0, XS_CODE_GET_VARIABLE, parser->errorSymbol);
			fxCoderAddByte(&coder, 2, XS_CODE_NEW);
			fxCoderAddString(&coder, 1, XS_CODE_STRING_1, mxStringLength(parser->errorMessage), parser->errorMessage);
			fxCoderAddInteger(&coder, -3, XS_CODE_RUN_1, 1);            fxCoderAddByte(&coder, -1, XS_CODE_THROW);
		}
		else
		{
			fxNRParserCodegen = savedCodegen; /* R26f */
			return C_NULL;
		}
	}
	
	fxCoderOptimize(&coder);
	
	script = c_malloc(sizeof(txScript));
	if (!script) goto bail;
	c_memset(script, 0, sizeof(txScript));
	
	code = coder.firstCode;
	size = 0;
	delta = 0;
	while (code) {
		txInteger value;
		switch (code->id) {
		case XS_NO_CODE:
			((txTargetCode*)code)->offset = size;
			break;
		case XS_CODE_BRANCH_1:
		case XS_CODE_BRANCH_CHAIN_1:
		case XS_CODE_BRANCH_COALESCE_1:
		case XS_CODE_BRANCH_ELSE_1:
		case XS_CODE_BRANCH_IF_1:
		case XS_CODE_BRANCH_STATUS_1:
		case XS_CODE_CATCH_1:
		case XS_CODE_CODE_1:
			size += 2;
			delta += 3;
			break;
			
		case XS_CODE_ARGUMENT:
		case XS_CODE_ARGUMENTS:
		case XS_CODE_ARGUMENTS_SLOPPY:
		case XS_CODE_ARGUMENTS_STRICT:
		case XS_CODE_BEGIN_SLOPPY:
		case XS_CODE_BEGIN_STRICT:
		case XS_CODE_BEGIN_STRICT_BASE:
		case XS_CODE_BEGIN_STRICT_DERIVED:
		case XS_CODE_BEGIN_STRICT_FIELD:
		case XS_CODE_MODULE:
			size += 2;
			break;

		case XS_CODE_LINE:
			size += 3;
			break;
		case XS_CODE_ASYNC_FUNCTION:
		case XS_CODE_ASYNC_GENERATOR_FUNCTION:
		case XS_CODE_CONSTRUCTOR_FUNCTION:
		case XS_CODE_DELETE_PROPERTY:
		case XS_CODE_DELETE_SUPER:
		case XS_CODE_FILE:
		case XS_CODE_FUNCTION:
		case XS_CODE_GENERATOR_FUNCTION:
		case XS_CODE_GET_PROPERTY:
		case XS_CODE_GET_SUPER:
		case XS_CODE_GET_THIS_VARIABLE:
		case XS_CODE_GET_VARIABLE:
		case XS_CODE_EVAL_PRIVATE:
		case XS_CODE_EVAL_REFERENCE:
		case XS_CODE_NAME:
		case XS_CODE_NEW_CLOSURE:
		case XS_CODE_NEW_LOCAL:
		case XS_CODE_NEW_PROPERTY:
		case XS_CODE_PROGRAM_REFERENCE:
		case XS_CODE_SET_PROPERTY:
		case XS_CODE_SET_SUPER:
		case XS_CODE_SET_VARIABLE:
		case XS_CODE_SYMBOL:
		case XS_CODE_PROFILE:
			size += 1 + sizeof(txID);
			break;
			
		case XS_CODE_STRING_1:
			size += ((txStringCode*)code)->length;
			// continue
			mxFallThrough;
		case XS_CODE_RESERVE_1:
		case XS_CODE_RETRIEVE_1:
		case XS_CODE_UNWIND_1:
			value = ((txIndexCode*)code)->index;
			if (value > 65535) {
				code->id += 2;
				size += 5;
			}
			else if (value > 255) {
				code->id += 1;
				size += 3;
			}
			else
				size += 2;
			break;
		case XS_CODE_BIGINT_1:
			value = fxBigIntMeasure(&((txBigIntCode*)code)->bigint);
			if (value > 255) {
				code->id += 1;
				size += 3;
			}
			else
				size += 2;
			size += value;
			break;
			
		case XS_CODE_CONST_CLOSURE_1:
		case XS_CODE_CONST_LOCAL_1:
		case XS_CODE_GET_CLOSURE_1:
		case XS_CODE_GET_LOCAL_1:
		case XS_CODE_GET_PRIVATE_1:
		case XS_CODE_HAS_PRIVATE_1:
		case XS_CODE_LET_CLOSURE_1:
		case XS_CODE_LET_LOCAL_1:
		case XS_CODE_NEW_PRIVATE_1:
		case XS_CODE_PULL_CLOSURE_1:
		case XS_CODE_PULL_LOCAL_1:
		case XS_CODE_REFRESH_CLOSURE_1:
		case XS_CODE_REFRESH_LOCAL_1:
		case XS_CODE_RESET_CLOSURE_1:
		case XS_CODE_RESET_LOCAL_1:
		case XS_CODE_SET_CLOSURE_1:
		case XS_CODE_SET_LOCAL_1:
		case XS_CODE_SET_PRIVATE_1:
		case XS_CODE_STORE_1:
		case XS_CODE_USED_1:
		case XS_CODE_VAR_CLOSURE_1:
		case XS_CODE_VAR_LOCAL_1:
			value = ((txIndexCode*)code)->index + 1;
			if (value > 65535) {
				code->id += 2;
				size += 5;
			}
			else if (value > 255) {
				code->id += 1;
				size += 3;
			}
			else
				size += 2;
			break;
			
		case XS_CODE_INTEGER_1: 
		case XS_CODE_RUN_1: 
		case XS_CODE_RUN_TAIL_1: 
			value = ((txIntegerCode*)code)->integer;
			if ((value < -32768) || (value > 32767)) {
				code->id += 2;
				size += 5;
			}
			else if ((value < -128) || (value > 127)) {
				code->id += 1;
				size += 3;
			}
			else
				size += 2;
			break;
		case XS_CODE_NUMBER:
			size += 9;
			break;
			
		case XS_CODE_HOST:
			size += 3;
			break;
			
		default:
			size++;
			break;
		}
		code = code->nextCode;
#ifdef mxMetering
		meterIndex++;
#endif
	}	
#ifdef mxMetering
	fxMeterSome(parser->console, meterIndex);
#endif

	code = coder.firstCode;
	size = 0;
	while (code) {
		switch (code->id) {
		case XS_NO_CODE:
			((txTargetCode*)code)->offset = size;
			break;
		case XS_CODE_BRANCH_1:
		case XS_CODE_BRANCH_CHAIN_1:
		case XS_CODE_BRANCH_COALESCE_1:
		case XS_CODE_BRANCH_ELSE_1:
		case XS_CODE_BRANCH_IF_1:
		case XS_CODE_BRANCH_STATUS_1:
		case XS_CODE_CATCH_1:
		case XS_CODE_CODE_1:
			offset = ((txBranchCode*)code)->target->offset - (size + 5);
			if ((offset < -32768) || (offset + delta > 32767)) {
				code->id += 2;
				size += 5;
			}
			else if ((offset < -128) || (offset + delta > 127)) {
				code->id += 1;
				delta -= 2;
				size += 3;
			}
			else {
				delta -= 3;
				size += 2;
			}
			break;
			
		case XS_CODE_ARGUMENT:
		case XS_CODE_ARGUMENTS:
		case XS_CODE_ARGUMENTS_SLOPPY:
		case XS_CODE_ARGUMENTS_STRICT:
		case XS_CODE_BEGIN_SLOPPY:
		case XS_CODE_BEGIN_STRICT:
		case XS_CODE_BEGIN_STRICT_BASE:
		case XS_CODE_BEGIN_STRICT_DERIVED:
		case XS_CODE_BEGIN_STRICT_FIELD:
		case XS_CODE_MODULE:
			size += 2;
			break;
		case XS_CODE_LINE:
			size += 3;
			break;
		case XS_CODE_ASYNC_FUNCTION:
		case XS_CODE_ASYNC_GENERATOR_FUNCTION:
		case XS_CODE_CONSTRUCTOR_FUNCTION:
		case XS_CODE_DELETE_PROPERTY:
		case XS_CODE_DELETE_SUPER:
		case XS_CODE_FILE:
		case XS_CODE_FUNCTION:
		case XS_CODE_GENERATOR_FUNCTION:
		case XS_CODE_GET_PROPERTY:
		case XS_CODE_GET_SUPER:
		case XS_CODE_GET_THIS_VARIABLE:
		case XS_CODE_GET_VARIABLE:
		case XS_CODE_EVAL_PRIVATE:
		case XS_CODE_EVAL_REFERENCE:
		case XS_CODE_NAME:
		case XS_CODE_NEW_CLOSURE:
		case XS_CODE_NEW_LOCAL:
		case XS_CODE_NEW_PROPERTY:
		case XS_CODE_PROGRAM_REFERENCE:
		case XS_CODE_SET_PROPERTY:
		case XS_CODE_SET_SUPER:
		case XS_CODE_SET_VARIABLE:
		case XS_CODE_SYMBOL:
			symbol = ((txSymbolCode*)code)->symbol;
			if (symbol && symbol->string)
				symbol->usage |= 1;
			size += 1 + sizeof(txID);
			break;
		case XS_CODE_PROFILE:
			size += 1 + sizeof(txID);
			break;
			
		case XS_CODE_CONST_CLOSURE_1:
		case XS_CODE_CONST_LOCAL_1:
		case XS_CODE_GET_CLOSURE_1:
		case XS_CODE_GET_LOCAL_1:
		case XS_CODE_GET_PRIVATE_1:
		case XS_CODE_HAS_PRIVATE_1:
		case XS_CODE_LET_CLOSURE_1:
		case XS_CODE_LET_LOCAL_1:
		case XS_CODE_NEW_PRIVATE_1:
		case XS_CODE_PULL_CLOSURE_1:
		case XS_CODE_PULL_LOCAL_1:
		case XS_CODE_REFRESH_CLOSURE_1:
		case XS_CODE_REFRESH_LOCAL_1:
		case XS_CODE_RESERVE_1:
		case XS_CODE_RESET_CLOSURE_1:
		case XS_CODE_RESET_LOCAL_1:
		case XS_CODE_RETRIEVE_1:
		case XS_CODE_SET_CLOSURE_1:
		case XS_CODE_SET_LOCAL_1:
		case XS_CODE_SET_PRIVATE_1:
		case XS_CODE_STORE_1:
		case XS_CODE_UNWIND_1:
		case XS_CODE_USED_1:
		case XS_CODE_VAR_CLOSURE_1:
		case XS_CODE_VAR_LOCAL_1:
			size += 2;
			break;
		case XS_CODE_CONST_CLOSURE_2:
		case XS_CODE_CONST_LOCAL_2:
		case XS_CODE_GET_CLOSURE_2:
		case XS_CODE_GET_LOCAL_2:
		case XS_CODE_GET_PRIVATE_2:
		case XS_CODE_HAS_PRIVATE_2:
		case XS_CODE_LET_CLOSURE_2:
		case XS_CODE_LET_LOCAL_2:
		case XS_CODE_NEW_PRIVATE_2:
		case XS_CODE_PULL_CLOSURE_2:
		case XS_CODE_PULL_LOCAL_2:
		case XS_CODE_REFRESH_CLOSURE_2:
		case XS_CODE_REFRESH_LOCAL_2:
		case XS_CODE_RESERVE_2:
		case XS_CODE_RESET_CLOSURE_2:
		case XS_CODE_RESET_LOCAL_2:
		case XS_CODE_RETRIEVE_2:
		case XS_CODE_SET_CLOSURE_2:
		case XS_CODE_SET_LOCAL_2:
		case XS_CODE_SET_PRIVATE_2:
		case XS_CODE_STORE_2:
		case XS_CODE_UNWIND_2:
		case XS_CODE_USED_2:
		case XS_CODE_VAR_CLOSURE_2:
		case XS_CODE_VAR_LOCAL_2:
			size += 3;
			break;
		
		case XS_CODE_INTEGER_1: 
		case XS_CODE_RUN_1: 
		case XS_CODE_RUN_TAIL_1: 
			size += 2;
			break;
		case XS_CODE_INTEGER_2: 
		case XS_CODE_RUN_2: 
		case XS_CODE_RUN_TAIL_2: 
			size += 3;
			break;
		case XS_CODE_INTEGER_4: 
		case XS_CODE_RUN_4: 
		case XS_CODE_RUN_TAIL_4: 
			size += 5;
			break;
		case XS_CODE_NUMBER:
			size += 9;
			break;
		case XS_CODE_STRING_1:
			size += 2 + ((txStringCode*)code)->length;
			break;
		case XS_CODE_STRING_2:
			size += 3 + ((txStringCode*)code)->length;
			break;
		case XS_CODE_STRING_4:
			size += 5 + ((txStringCode*)code)->length;
			break;
		case XS_CODE_BIGINT_1:
			size += 2 + fxBigIntMeasure(&((txBigIntCode*)code)->bigint);
			break;
		case XS_CODE_BIGINT_2:
			size += 3 + fxBigIntMeasure(&((txBigIntCode*)code)->bigint);
			break;
			
		case XS_CODE_HOST:
			size += 3;
			break;
		
		default:
			size++;
			break;
		}
		code = code->nextCode;
	}	
	
	node = parser->firstHostNode;
	while (node) {
		if (node->symbol)
			node->symbol->usage |= 1;
		node = node->nextHostNode;
	}
	
	address = parser->symbolTable;
	c = parser->symbolModulo;
	id = 1;
	total = sizeof(txID);
	for (i = 0; i < c; i++) {
		txSymbol* symbol = *address;
		while (symbol) {
			if (symbol->usage & 1) {
				symbol->ID = id;
				id++;
				total += symbol->length;
			}
			symbol = symbol->next;
		}
		address++;
	}
	count = id;
		
	script->codeBuffer = c_malloc(size);
	if (!script->codeBuffer) goto bail;
	script->codeSize = size;
	
	code = coder.firstCode;
	p = script->codeBuffer;
	while (code) {
		txS1 s1; txS2 s2; txS4 s4; 
		txU1 u1; txU2 u2;
		txNumber n;
		if (code->id)
			*p++ = (txS1)(code->id);
		switch (code->id) {
		case XS_CODE_BRANCH_1:
		case XS_CODE_BRANCH_CHAIN_1:
		case XS_CODE_BRANCH_COALESCE_1:
		case XS_CODE_BRANCH_ELSE_1:
		case XS_CODE_BRANCH_IF_1:
		case XS_CODE_BRANCH_STATUS_1:
		case XS_CODE_CATCH_1:
		case XS_CODE_CODE_1:
			offset = mxPtrDiff(p + 1 - script->codeBuffer);
			s1 = (txS1)(((txBranchCode*)code)->target->offset - offset);
			*p++ = s1;
			break;
		case XS_CODE_BRANCH_2:
		case XS_CODE_BRANCH_CHAIN_2:
		case XS_CODE_BRANCH_COALESCE_2:
		case XS_CODE_BRANCH_ELSE_2:
		case XS_CODE_BRANCH_IF_2:
		case XS_CODE_BRANCH_STATUS_2:
		case XS_CODE_CATCH_2:
		case XS_CODE_CODE_2:
			offset = mxPtrDiff(p + 2 - script->codeBuffer);
			s2 = (txS2)(((txBranchCode*)code)->target->offset - offset);
			mxEncode2(p, s2);
			break;
		case XS_CODE_BRANCH_4:
		case XS_CODE_BRANCH_CHAIN_4:
		case XS_CODE_BRANCH_COALESCE_4:
		case XS_CODE_BRANCH_ELSE_4:
		case XS_CODE_BRANCH_IF_4:
		case XS_CODE_BRANCH_STATUS_4:
		case XS_CODE_CATCH_4:
		case XS_CODE_CODE_4:
			offset = mxPtrDiff(p + 4 - script->codeBuffer);
			s4 = (txS4)(((txBranchCode*)code)->target->offset  - offset);
			mxEncode4(p, s4);
			break;
			
		case XS_CODE_ASYNC_FUNCTION:
		case XS_CODE_ASYNC_GENERATOR_FUNCTION:
		case XS_CODE_CONSTRUCTOR_FUNCTION:
		case XS_CODE_DELETE_PROPERTY:
		case XS_CODE_DELETE_SUPER:
		case XS_CODE_FILE:
		case XS_CODE_FUNCTION:
		case XS_CODE_GENERATOR_FUNCTION:
		case XS_CODE_GET_PROPERTY:
		case XS_CODE_GET_SUPER:
		case XS_CODE_GET_THIS_VARIABLE:
		case XS_CODE_GET_VARIABLE:
		case XS_CODE_EVAL_PRIVATE:
		case XS_CODE_EVAL_REFERENCE:
		case XS_CODE_NAME:
		case XS_CODE_NEW_CLOSURE:
		case XS_CODE_NEW_LOCAL:
		case XS_CODE_NEW_PROPERTY:
		case XS_CODE_PROGRAM_REFERENCE:
		case XS_CODE_SET_PROPERTY:
		case XS_CODE_SET_SUPER:
		case XS_CODE_SET_VARIABLE:
		case XS_CODE_SYMBOL:
			symbol = ((txSymbolCode*)code)->symbol;
			if (symbol && symbol->string)
				id = symbol->ID;
			else
				id = XS_NO_ID;
			mxEncodeID(p, id);
			break;
		case XS_CODE_PROFILE:
			id = fxGenerateProfileID(parser->console);
			mxEncodeID(p, id);
			break;
			
		case XS_CODE_ARGUMENT:
		case XS_CODE_ARGUMENTS:
		case XS_CODE_ARGUMENTS_SLOPPY:
		case XS_CODE_ARGUMENTS_STRICT:
		case XS_CODE_BEGIN_SLOPPY:
		case XS_CODE_BEGIN_STRICT:
		case XS_CODE_BEGIN_STRICT_BASE:
		case XS_CODE_BEGIN_STRICT_DERIVED:
		case XS_CODE_BEGIN_STRICT_FIELD:
		case XS_CODE_MODULE:
		case XS_CODE_RESERVE_1:
		case XS_CODE_RETRIEVE_1:
		case XS_CODE_UNWIND_1:
			u1 = (txU1)(((txIndexCode*)code)->index);
			*((txU1*)p++) = u1;
			break;
		case XS_CODE_LINE:
		case XS_CODE_RESERVE_2:
		case XS_CODE_RETRIEVE_2:
		case XS_CODE_UNWIND_2:
			u2 = (txU2)(((txIndexCode*)code)->index);
			mxEncode2(p, u2);
			break;

		case XS_CODE_CONST_CLOSURE_1:
		case XS_CODE_CONST_LOCAL_1:
		case XS_CODE_GET_CLOSURE_1:
		case XS_CODE_GET_LOCAL_1:
		case XS_CODE_GET_PRIVATE_1:
		case XS_CODE_HAS_PRIVATE_1:
		case XS_CODE_LET_CLOSURE_1:
		case XS_CODE_LET_LOCAL_1:
		case XS_CODE_NEW_PRIVATE_1:
		case XS_CODE_PULL_CLOSURE_1:
		case XS_CODE_PULL_LOCAL_1:
		case XS_CODE_REFRESH_CLOSURE_1:
		case XS_CODE_REFRESH_LOCAL_1:
		case XS_CODE_RESET_CLOSURE_1:
		case XS_CODE_RESET_LOCAL_1:
		case XS_CODE_SET_CLOSURE_1:
		case XS_CODE_SET_LOCAL_1:
		case XS_CODE_SET_PRIVATE_1:
		case XS_CODE_STORE_1:
		case XS_CODE_USED_1:
		case XS_CODE_VAR_CLOSURE_1:
		case XS_CODE_VAR_LOCAL_1:
			u1 = (txU1)(((txIndexCode*)code)->index + 1);
			*((txU1*)p++) = u1;
			break;

		case XS_CODE_CONST_CLOSURE_2:
		case XS_CODE_CONST_LOCAL_2:
		case XS_CODE_GET_CLOSURE_2:
		case XS_CODE_GET_LOCAL_2:
		case XS_CODE_GET_PRIVATE_2:
		case XS_CODE_HAS_PRIVATE_2:
		case XS_CODE_LET_CLOSURE_2:
		case XS_CODE_LET_LOCAL_2:
		case XS_CODE_NEW_PRIVATE_2:
		case XS_CODE_PULL_CLOSURE_2:
		case XS_CODE_PULL_LOCAL_2:
		case XS_CODE_REFRESH_CLOSURE_2:
		case XS_CODE_REFRESH_LOCAL_2:
		case XS_CODE_RESET_CLOSURE_2:
		case XS_CODE_RESET_LOCAL_2:
		case XS_CODE_SET_CLOSURE_2:
		case XS_CODE_SET_LOCAL_2:
		case XS_CODE_SET_PRIVATE_2:
		case XS_CODE_STORE_2:
		case XS_CODE_USED_2:
		case XS_CODE_VAR_CLOSURE_2:
		case XS_CODE_VAR_LOCAL_2:
			u2 = (txU2)(((txIndexCode*)code)->index + 1);
			mxEncode2(p, u2);
			break;
	
		case XS_CODE_INTEGER_1: 
		case XS_CODE_RUN_1: 
		case XS_CODE_RUN_TAIL_1: 
			s1 = (txS1)(((txIntegerCode*)code)->integer);
			*p++ = s1;
			break;
		case XS_CODE_INTEGER_2: 
		case XS_CODE_RUN_2: 
		case XS_CODE_RUN_TAIL_2: 
			s2 = (txS2)(((txIntegerCode*)code)->integer);
			mxEncode2(p, s2);
			break;
		case XS_CODE_INTEGER_4: 
		case XS_CODE_RUN_4: 
		case XS_CODE_RUN_TAIL_4: 
			s4 = (txS4)(((txIntegerCode*)code)->integer);
			mxEncode4(p, s4);
			break;
		case XS_CODE_NUMBER:
			n = ((txNumberCode*)code)->number;
			mxEncode8(p, n);
			break;
		case XS_CODE_STRING_1:
			u1 = (txU1)(((txStringCode*)code)->length);
			*((txU1*)p++) = u1;
			c_memcpy(p, ((txStringCode*)code)->string, u1);
			p += u1;
			break;
		case XS_CODE_STRING_2:
			u2 = (txU2)(((txStringCode*)code)->length);
			mxEncode2(p, u2);
			c_memcpy(p, ((txStringCode*)code)->string, u2);
			p += u2;
			break;
		case XS_CODE_STRING_4:
			s4 = (txS4)(((txStringCode*)code)->length);
			mxEncode4(p, s4);
			c_memcpy(p, ((txStringCode*)code)->string, s4);
			p += s4;
			break;
		case XS_CODE_BIGINT_1:
			u1 = (txU1)fxBigIntMeasure(&((txBigIntCode*)code)->bigint);
			*((txU1*)p++) = u1;
			fxBigIntEncode(p, &((txBigIntCode*)code)->bigint, u1);
			p += u1;
			break;
		case XS_CODE_BIGINT_2:
			u2 = (txU2)fxBigIntMeasure(&((txBigIntCode*)code)->bigint);
            mxEncode2(p, u2);
			fxBigIntEncode(p, &((txBigIntCode*)code)->bigint, u2);
			p += u2;
			break;
			
		case XS_CODE_HOST:
			u2 = (txU2)(((txIndexCode*)code)->index);
			mxEncode2(p, u2);
			break;
		}
		code = code->nextCode;
	}	
	
#ifdef mxCodePrint
	fprintf(stderr, "\n");
	code = coder.firstCode;
	while (code) {
		txInteger tab;
		for (tab = 0; tab < code->stackLevel; tab++)
			fprintf(stderr, "\t");
		switch (code->id) {
		case XS_NO_CODE:
			fprintf(stderr, "_%d\n", ((txTargetCode*)code)->index);
			break;
		
		case XS_CODE_BRANCH_1:
		case XS_CODE_BRANCH_2:
		case XS_CODE_BRANCH_4:
		case XS_CODE_BRANCH_CHAIN_1:
		case XS_CODE_BRANCH_CHAIN_2:
		case XS_CODE_BRANCH_CHAIN_4:
		case XS_CODE_BRANCH_COALESCE_1:
		case XS_CODE_BRANCH_COALESCE_2:
		case XS_CODE_BRANCH_COALESCE_4:
		case XS_CODE_BRANCH_ELSE_1:
		case XS_CODE_BRANCH_ELSE_2:
		case XS_CODE_BRANCH_ELSE_4:
		case XS_CODE_BRANCH_IF_1:
		case XS_CODE_BRANCH_IF_2:
		case XS_CODE_BRANCH_IF_4:
		case XS_CODE_BRANCH_STATUS_1:
		case XS_CODE_BRANCH_STATUS_2:
		case XS_CODE_BRANCH_STATUS_4:
		case XS_CODE_CATCH_1:
		case XS_CODE_CATCH_2:
		case XS_CODE_CATCH_4:
		case XS_CODE_CODE_1:
		case XS_CODE_CODE_2:
		case XS_CODE_CODE_4:
			fprintf(stderr, "%s _%d\n", gxCodeNames[code->id], ((txBranchCode*)code)->target->index);
			break;
		
		case XS_CODE_ARGUMENT:
		case XS_CODE_ARGUMENTS:
		case XS_CODE_ARGUMENTS_SLOPPY:
		case XS_CODE_ARGUMENTS_STRICT:
		case XS_CODE_BEGIN_SLOPPY:
		case XS_CODE_BEGIN_STRICT:
		case XS_CODE_BEGIN_STRICT_BASE:
		case XS_CODE_BEGIN_STRICT_DERIVED:
		case XS_CODE_BEGIN_STRICT_FIELD:
		case XS_CODE_LINE:
		case XS_CODE_MODULE:
			fprintf(stderr, "%s %d\n", gxCodeNames[code->id], ((txIndexCode*)code)->index);
			break;
			
		case XS_CODE_ASYNC_FUNCTION:
		case XS_CODE_ASYNC_GENERATOR_FUNCTION:
		case XS_CODE_CONSTRUCTOR_FUNCTION:
		case XS_CODE_DELETE_PROPERTY:
		case XS_CODE_DELETE_SUPER:
		case XS_CODE_FILE:
		case XS_CODE_FUNCTION:
		case XS_CODE_GENERATOR_FUNCTION:
		case XS_CODE_GET_PROPERTY:
		case XS_CODE_GET_SUPER:
		case XS_CODE_GET_THIS_VARIABLE:
		case XS_CODE_GET_VARIABLE:
		case XS_CODE_EVAL_PRIVATE:
		case XS_CODE_EVAL_REFERENCE:
		case XS_CODE_NAME:
		case XS_CODE_NEW_PROPERTY:
		case XS_CODE_PROGRAM_REFERENCE:
		case XS_CODE_SET_PROPERTY:
		case XS_CODE_SET_SUPER:
		case XS_CODE_SET_VARIABLE:
		case XS_CODE_SYMBOL:
			symbol = ((txSymbolCode*)code)->symbol;
			if (symbol && symbol->string)
				fprintf(stderr, "%s %s\n", gxCodeNames[code->id], symbol->string);
			else
				fprintf(stderr, "%s ?\n", gxCodeNames[code->id]);
			break;
		
		case XS_CODE_CONST_CLOSURE_1:
		case XS_CODE_CONST_CLOSURE_2:
		case XS_CODE_CONST_LOCAL_1:
		case XS_CODE_CONST_LOCAL_2:
		case XS_CODE_GET_CLOSURE_1:
		case XS_CODE_GET_CLOSURE_2:
		case XS_CODE_GET_LOCAL_1:
		case XS_CODE_GET_LOCAL_2:
		case XS_CODE_GET_PRIVATE_1:
		case XS_CODE_GET_PRIVATE_2:
		case XS_CODE_HAS_PRIVATE_1:
		case XS_CODE_HAS_PRIVATE_2:
		case XS_CODE_LET_CLOSURE_1:
		case XS_CODE_LET_CLOSURE_2:
		case XS_CODE_LET_LOCAL_1:
		case XS_CODE_LET_LOCAL_2:
		case XS_CODE_NEW_PRIVATE_1:
		case XS_CODE_NEW_PRIVATE_2:
		case XS_CODE_PULL_CLOSURE_1:
		case XS_CODE_PULL_CLOSURE_2:
		case XS_CODE_PULL_LOCAL_1:
		case XS_CODE_PULL_LOCAL_2:
		case XS_CODE_REFRESH_CLOSURE_1:
		case XS_CODE_REFRESH_CLOSURE_2:
		case XS_CODE_REFRESH_LOCAL_1:
		case XS_CODE_REFRESH_LOCAL_2:
		case XS_CODE_RESET_CLOSURE_1:
		case XS_CODE_RESET_CLOSURE_2:
		case XS_CODE_RESET_LOCAL_1:
		case XS_CODE_RESET_LOCAL_2:
		case XS_CODE_SET_CLOSURE_1:
		case XS_CODE_SET_CLOSURE_2:
		case XS_CODE_SET_LOCAL_1:
		case XS_CODE_SET_LOCAL_2:
		case XS_CODE_SET_PRIVATE_1:
		case XS_CODE_SET_PRIVATE_2:
		case XS_CODE_STORE_1:
		case XS_CODE_STORE_2:
		case XS_CODE_USED_1:
		case XS_CODE_USED_2:
		case XS_CODE_VAR_CLOSURE_1:
		case XS_CODE_VAR_CLOSURE_2:
		case XS_CODE_VAR_LOCAL_1:
		case XS_CODE_VAR_LOCAL_2:
			fprintf(stderr, "%s [%d]\n", gxCodeNames[code->id], ((txIndexCode*)code)->index);
			break;
		
		case XS_CODE_RESERVE_1:
		case XS_CODE_RESERVE_2:
		case XS_CODE_UNWIND_1:
		case XS_CODE_UNWIND_2:
			fprintf(stderr, "%s #%d\n", gxCodeNames[code->id], ((txIndexCode*)code)->index);
			break;
		case XS_CODE_NEW_CLOSURE:
		case XS_CODE_NEW_LOCAL:
			fprintf(stderr, "[%d] %s %s\n", ((txVariableCode*)code)->index, gxCodeNames[code->id], ((txSymbolCode*)code)->symbol->string);
			break;
		case XS_CODE_NEW_TEMPORARY:
			fprintf(stderr, "[%d] %s\n", ((txIndexCode*)code)->index, gxCodeNames[code->id]);
			break;
		case XS_CODE_RETRIEVE_1:
		case XS_CODE_RETRIEVE_2:
			{
				txInteger i, c = ((txIndexCode*)code)->index;
				fprintf(stderr, "[0");
				for (i = 1; i < c; i++)
					fprintf(stderr, ",%d", i);
				fprintf(stderr, "] %s\n", gxCodeNames[code->id]);
			}
			break;
		
		case XS_CODE_INTEGER_1: 
		case XS_CODE_INTEGER_2: 
		case XS_CODE_INTEGER_4: 
		case XS_CODE_RUN_1: 
		case XS_CODE_RUN_2: 
		case XS_CODE_RUN_4: 
		case XS_CODE_RUN_TAIL_1: 
		case XS_CODE_RUN_TAIL_2: 
		case XS_CODE_RUN_TAIL_4: 
			fprintf(stderr, "%s %d\n", gxCodeNames[code->id], ((txIntegerCode*)code)->integer);
			break;
		case XS_CODE_NUMBER:
			fprintf(stderr, "%s %lf\n", gxCodeNames[code->id], ((txNumberCode*)code)->number);
			break;
		case XS_CODE_STRING_1:
		case XS_CODE_STRING_2:
		case XS_CODE_STRING_4:
			fprintf(stderr, "%s %d \"%s\"\n", gxCodeNames[code->id], ((txStringCode*)code)->length, ((txStringCode*)code)->string);
			break;
		case XS_CODE_BIGINT_1:
		case XS_CODE_BIGINT_2:
			fprintf(stderr, "%s %d\n", gxCodeNames[code->id], fxBigIntMeasure(&((txBigIntCode*)code)->bigint));
			break;
			
		case XS_CODE_HOST:
			fprintf(stderr, "%s %d\n", gxCodeNames[code->id], ((txIndexCode*)code)->index);
			break;
	
		default:
			fprintf(stderr, "%s\n", gxCodeNames[code->id]);
			break;
		}
		code = code->nextCode;
	}
#endif
	script->symbolsBuffer = c_malloc(total);
	if (!script->symbolsBuffer) goto bail;
	script->symbolsSize = total;
	
	p = script->symbolsBuffer;
	mxEncodeID(p, count);
	
	address = parser->symbolTable;
	c = parser->symbolModulo;
	/* R19: bound the whole-table walk; a corrupted bucket chain (cycle in
	   ->next) spins here forever (device watchdog freeze at seg ~62). On
	   blowout, log the stalled symbol name and abort containedly. */
	{
		unsigned long symWalkGuard = 0;
		for (i = 0; i < c; i++) {
			txSymbol* symbol = *address;
			while (symbol) {
				if (symbol->usage & 1) {
					c_memcpy(p, symbol->string, symbol->length);
					p += symbol->length;
				}
				symbol = symbol->next;
				if (++symWalkGuard > 65536) {
					extern long gXSNRFaultKind;
					extern void* gXSNRFaultFrame;
					extern char gXSNRFaultSym[];
					txString s = symbol ? symbol->string : (txString)NULL;
					txSize n = 0;
					gXSNRFaultKind = -200 - (long)i;
					gXSNRFaultFrame = symbol;
					if (s) { while (s[n] && (n < 31)) { gXSNRFaultSym[n] = s[n]; n++; } }
					gXSNRFaultSym[n] = 0;
					fxAbort(parser->console, XS_PUMP_CORRUPTION_EXIT);
				}
			}
			address++;
		}
	}
	
	c = (txS2)(parser->hostNodeIndex);
	if (c) {
		size = sizeof(txID);
		node = parser->firstHostNode;
		while (node) {
			size += 1 + sizeof(txID) + node->at->length + 1;
			node = node->nextHostNode;
		}
	
		script->hostsBuffer = c_malloc(size);
		if (!script->hostsBuffer) goto bail;
		script->hostsSize = size;
	
		p = script->hostsBuffer;
		mxEncodeID(p, c);
		node = parser->firstHostNode;
		while (node) {
			*p++ = (txS1)(node->paramsCount);
			if (node->symbol)
				c = node->symbol->ID;
			else
				c = XS_NO_ID;
			mxEncodeID(p, c);
			c_memcpy(p, node->at->value, node->at->length);
			p += node->at->length;
			*p++ = 0;
			node = node->nextHostNode;
		}
	}

	fxNRParserCodegen = savedCodegen; /* R26f: normal exit */
	return script;
bail:
	fxNRParserCodegen = savedCodegen; /* R26f: bail exit */
	fxDeleteScript(script);
	return C_NULL;
}

void fxCoderAdd(txCoder* self, txInteger delta, void* it)
{
	txByteCode* code = it;
	if (self->lastCode)
		self->lastCode->nextCode = code;
	else
		self->firstCode = code;
	self->lastCode = code;
	self->stackLevel += delta;
	code->stackLevel = self->stackLevel;
//	if (self->stackLevel < 0)
//		c_fprintf(stderr, "# oops %d\n", code->id);		//@@
}

void fxCoderAddBigInt(txCoder* self, txInteger delta, txInteger id, txBigInt* bigint)
{
	txBigIntCode* code = fxNewParserChunkClear(self->parser, sizeof(txBigIntCode));
	fxCoderAdd(self, delta, code);
	code->id = id;
	code->bigint = *bigint;
}

void fxCoderAddBranch(txCoder* self, txInteger delta, txInteger id, txTargetCode* target)
{
	txBranchCode* code = fxNewParserChunkClear(self->parser, sizeof(txBranchCode));
	fxCoderAdd(self, delta, code);
	code->id = id;
	code->target = target;
	target->used = 1;
}

void fxCoderAddByte(txCoder* self, txInteger delta, txInteger id)
{
	txByteCode* code = fxNewParserChunkClear(self->parser, sizeof(txByteCode));
	fxCoderAdd(self, delta, code);
	code->id = id;
}

void fxCoderAddIndex(txCoder* self, txInteger delta, txInteger id, txInteger index)
{
	txIndexCode* code = fxNewParserChunkClear(self->parser, sizeof(txIndexCode));
	fxCoderAdd(self, delta, code);
	code->id = id;
	code->index = index;
}

void fxCoderAddInteger(txCoder* self, txInteger delta, txInteger id, txInteger integer)
{
	txIntegerCode* code = fxNewParserChunkClear(self->parser, sizeof(txIntegerCode));
	fxCoderAdd(self, delta, code);
	code->id = id;
	code->integer = integer;
}

void fxCoderAddLine(txCoder* self, txInteger delta, txInteger id, txNode* node)
{
	if (self->parser->flags & mxDebugFlag) {
		if (self->parser->lines) {
			if (node->path != self->parser->path) {
				node->path = self->parser->path;
				node->line = self->parser->lines[node->line];
			}
		}
		else if (self->parser->source) {
			node->path = self->parser->source;
		}
		if (self->path != node->path) {
			if (node->path) {
				fxCoderAddSymbol(self, 0, XS_CODE_FILE, node->path);
				fxCoderAddIndex(self, 0, id, node->line);
			}
			self->path = node->path;
			self->line = node->line;
		}
		else if (self->line != node->line) {
			if (self->path) {
				txIndexCode* code = (txIndexCode*)self->lastCode;
				if (code && (code->id == id) && (code->index != 0))
					code->index = node->line;
				else
					fxCoderAddIndex(self, 0, id, node->line);
			}
			self->line = node->line;
		}
	}
}

void fxCoderAddNumber(txCoder* self, txInteger delta, txInteger id, txNumber number)
{
	txNumberCode* code = fxNewParserChunkClear(self->parser, sizeof(txNumberCode));
	fxCoderAdd(self, delta, code);
	code->id = id;
	code->number = number;
}

void fxCoderAddString(txCoder* self, txInteger delta, txInteger id, txInteger length, txString string)
{
	txStringCode* code = fxNewParserChunkClear(self->parser, sizeof(txStringCode));
	fxCoderAdd(self, delta, code);
	code->id = id;
	code->length = length + 1;
	code->string = string;
}

void fxCoderAddSymbol(txCoder* self, txInteger delta, txInteger id, txSymbol* symbol)
{
	txSymbolCode* code = fxNewParserChunkClear(self->parser, sizeof(txSymbolCode));
	fxCoderAdd(self, delta, code);
	code->id = id;
	code->symbol = symbol;
}

void fxCoderAddVariable(txCoder* self, txInteger delta, txInteger id, txSymbol* symbol, txInteger index)
{
	txVariableCode* code = fxNewParserChunkClear(self->parser, sizeof(txVariableCode));
	fxCoderAdd(self, delta, code);
	code->id = id;
	code->symbol = symbol;
	code->index = index;
}

void fxCoderAdjustEnvironment(txCoder* self, txTargetCode* target)
{
	txInteger count = self->environmentLevel - target->environmentLevel;
	while (count) {
		fxCoderAddByte(self, 0, XS_CODE_WITHOUT);
		count--;
	}
}

void fxCoderAdjustScope(txCoder* self, txTargetCode* target)
{
	txInteger count = self->scopeLevel - target->scopeLevel;
	if (count)
		fxCoderAddIndex(self, 0, XS_CODE_UNWIND_1, count);
}

txTargetCode* fxCoderAliasTargets(txCoder* self, txTargetCode* target)
{
	txTargetCode* result = NULL;
	if (target) {
		txTargetCode* alias = result = fxNewParserChunkClear(self->parser, sizeof(txTargetCode));
		alias->index = self->targetIndex++;
		alias->label = target->label;
		alias->environmentLevel = self->environmentLevel;
		alias->scopeLevel = self->scopeLevel;
		alias->stackLevel = self->stackLevel;
		alias->original = target;
		target = target->nextTarget;
		while (target) {
			alias = alias->nextTarget = fxNewParserChunkClear(self->parser, sizeof(txTargetCode));
			alias->index = self->targetIndex++;
			alias->label = target->label;
			alias->environmentLevel = self->environmentLevel;
			alias->scopeLevel = self->scopeLevel;
			alias->stackLevel = self->stackLevel;
			alias->original = target;
			target = target->nextTarget;
		}
	}
	return result;
}

txInteger fxCoderCountParameters(txCoder* self, txNode* params)
{
	txNode* item = ((txParamsBindingNode*)params)->items->first;
	txInteger count = 0;
	while (item) {
		if (item->description->token == XS_TOKEN_REST_BINDING)
			break;
		if ((item->description->token != XS_TOKEN_ARG) && (item->description->token != XS_TOKEN_ARRAY_BINDING) && (item->description->token != XS_TOKEN_OBJECT_BINDING))
			break;
		count++;
		item = item->next;
	}
	return count;
}

txTargetCode* fxCoderCreateTarget(txCoder* self)
{
	txTargetCode* result = fxNewParserChunkClear(self->parser, sizeof(txTargetCode));
	result->index = self->targetIndex++;
	result->environmentLevel = self->environmentLevel;
	result->scopeLevel = self->scopeLevel;
	result->stackLevel = self->stackLevel;
	return result;
}

txTargetCode* fxCoderFinalizeTargets(txCoder* self, txTargetCode* alias, txInteger selector, txInteger* address, txTargetCode* finallyTarget)
{
	txTargetCode* result = NULL;
	txInteger selection = *address;
	if (alias) {
		result = alias->original;
		while (alias) {
			if (alias->used) {
				fxCoderAdd(self, 0, alias);
				fxCoderAddInteger(self, 1, XS_CODE_INTEGER_1, selection);
				fxCoderAddIndex(self, -1, XS_CODE_PULL_LOCAL_1, selector);
				fxCoderAddBranch(self, 0, XS_CODE_BRANCH_1, finallyTarget);
				alias->original->used = 1;
			}
			alias = alias->nextTarget;
			selection++;
		}
	}
	*address = selection;
	return result;
}

void fxCoderJumpTargets(txCoder* self, txTargetCode* target, txInteger selector, txInteger* address)
{
	txInteger selection = *address;
	while (target) {
		if (target->used) {
			txTargetCode* elseTarget = fxCoderCreateTarget(self);
			fxCoderAddInteger(self, 1, XS_CODE_INTEGER_1, selection);
			fxCoderAddIndex(self, 1, XS_CODE_GET_LOCAL_1, selector);
			fxCoderAddByte(self, -1, XS_CODE_STRICT_EQUAL);
			fxCoderAddBranch(self, -1, XS_CODE_BRANCH_ELSE_1, elseTarget);
			fxCoderAdjustEnvironment(self, target);
			fxCoderAdjustScope(self, target);
			fxCoderAddBranch(self, 0, XS_CODE_BRANCH_1, target);
			fxCoderAdd(self, 0, elseTarget);
		}
		target = target->nextTarget;
		selection++;
	}
	*address = selection;
}

void fxCoderOptimize(txCoder* self)
{
	txByteCode** address;
	txByteCode* code;
	
	// branch to (target | unwind)* end => end
	address = &self->firstCode;
	while ((code = *address)) {
		if (code->id == XS_CODE_BRANCH_1) {
			txByteCode* nextCode = ((txBranchCode*)code)->target->nextCode;
			while ((nextCode->id == XS_NO_CODE) || (nextCode->id == XS_CODE_UNWIND_1))
				nextCode = nextCode->nextCode;
			if ((XS_CODE_END <= nextCode->id) && (nextCode->id <= XS_CODE_END_DERIVED)) {
				txByteCode* end = fxNewParserChunkClear(self->parser, sizeof(txByteCode));
				end->nextCode = code->nextCode;
				end->id = nextCode->id;
				end->stackLevel = code->stackLevel;
				*address = end;
			}
			else
				address = &code->nextCode;
		}
		else
			address = &code->nextCode;
	}
	// unwind (target | unwind)* end => (target | unwind)* end
	address = &self->firstCode;
	while ((code = *address)) {
		if (code->id == XS_CODE_UNWIND_1) {
			txByteCode* nextCode = code->nextCode;
			while ((nextCode->id == XS_NO_CODE) || (nextCode->id == XS_CODE_UNWIND_1))
				nextCode = nextCode->nextCode;
			if ((XS_CODE_END <= nextCode->id) && (nextCode->id <= XS_CODE_END_DERIVED))
				*address = code->nextCode;
			else
				address = &code->nextCode;
		}
		else
			address = &code->nextCode;
	}
	// end target* end => target* end
	address = &self->firstCode;
	while ((code = *address)) {
		if ((XS_CODE_END <= code->id) && (code->id <= XS_CODE_END_DERIVED)) {
			txByteCode* nextCode = code->nextCode;
			if (!nextCode)
				break;
			while (nextCode->id == XS_NO_CODE)
				nextCode = nextCode->nextCode;
			if (nextCode->id == code->id)
				*address = code->nextCode;
			else
				address = &code->nextCode;
		}
		else
			address = &code->nextCode;
	}
	// branch to next =>
	address = &self->firstCode;
	while ((code = *address)) {
		if (code->id == XS_CODE_BRANCH_1) {
			if (code->nextCode == (txByteCode*)(((txBranchCode*)code)->target))
				*address = code->nextCode;
			else
				address = &code->nextCode;
		}
		else
			address = &code->nextCode;
	}
}

txInteger fxCoderUseTemporaryVariable(txCoder* self)
{
	txInteger result = self->scopeLevel++;
	fxCoderAddIndex(self, 0, XS_CODE_NEW_TEMPORARY, result);
	return result;
}

void fxCoderUnuseTemporaryVariables(txCoder* self, txInteger count)
{
	fxCoderAddIndex(self, 0, XS_CODE_UNWIND_1, count);
	self->scopeLevel -= count;
}

void fxScopeCoded(txScope* self, txCoder* coder) 
{
	if (self->declareNodeCount) {
		if (self->flags & mxEvalFlag) {
			coder->environmentLevel--;
			fxCoderAddByte(coder, 0, XS_CODE_WITHOUT);
		}
		fxCoderAddIndex(coder, 0, XS_CODE_UNWIND_1, self->declareNodeCount);
		coder->scopeLevel -= self->declareNodeCount;
	}
}

void fxScopeCodedBody(txScope* self, txCoder* coder) 
{
	txScope* functionScope = self->scope;
	if ((functionScope->node->flags & mxEvalFlag) && !(functionScope->node->flags & mxStrictFlag)) {
		coder->environmentLevel--;
		fxCoderAddByte(coder, 0, XS_CODE_WITHOUT);
		coder->environmentLevel--;
		fxCoderAddByte(coder, 0, XS_CODE_WITHOUT);
		fxCoderAddIndex(coder, 0, XS_CODE_UNWIND_1, self->declareNodeCount);
		coder->scopeLevel -= self->declareNodeCount;
	}
	else 
		fxScopeCoded(self, coder);
}

void fxScopeCodingBlock(txScope* self, txCoder* coder) 
{
	if (self->declareNodeCount) {
		txDeclareNode* node = self->firstDeclareNode;
		while (node) {
			if (node->flags & mxDeclareNodeClosureFlag) {
				if (!(node->flags & mxDeclareNodeUseClosureFlag)) {
					node->index = coder->scopeLevel++;
					fxCoderAddVariable(coder, 0, XS_CODE_NEW_CLOSURE, node->symbol, node->index);
					if (node->description->token == XS_TOKEN_VAR) {
						fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
						fxCoderAddIndex(coder, 0, XS_CODE_VAR_CLOSURE_1, node->index);
						fxCoderAddByte(coder, -1, XS_CODE_POP);
					}
				}
			}
			else {
				node->index = coder->scopeLevel++;
				if (node->symbol)
					fxCoderAddVariable(coder, 0, XS_CODE_NEW_LOCAL, node->symbol, node->index);
				else
					fxCoderAddIndex(coder, 0, XS_CODE_NEW_TEMPORARY, node->index);
				if (node->description->token == XS_TOKEN_VAR) {
					fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
					fxCoderAddIndex(coder, 0, XS_CODE_VAR_LOCAL_1, node->index);
					fxCoderAddByte(coder, -1, XS_CODE_POP);
				}
			}
			node = node->nextDeclareNode;
		}
		if (self->flags & mxEvalFlag) {
			fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
			fxCoderAddByte(coder, 0, XS_CODE_WITH);
			node = self->firstDeclareNode;
			while (node) {
				fxCoderAddIndex(coder, 0, XS_CODE_STORE_1, node->index);
				node = node->nextDeclareNode;
			}
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			coder->environmentLevel++;
		}
	}
}

void fxScopeCodingBody(txScope* self, txCoder* coder) 
{
	if ((self->node->flags & mxEvalFlag) && !(self->node->flags & mxStrictFlag)) {
		txDeclareNode* node = self->firstDeclareNode;
		while (node) {
			if (node->description->token == XS_TOKEN_DEFINE) {
				node->index = coder->scopeLevel++;
				fxCoderAddVariable(coder, 0, XS_CODE_NEW_CLOSURE, node->symbol, node->index);
				fxCoderAddByte(coder, 1, XS_CODE_NULL);
				fxCoderAddIndex(coder, 0, XS_CODE_VAR_CLOSURE_1, node->index);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
			}
			else if (node->description->token == XS_TOKEN_VAR) {
				node->index = coder->scopeLevel++;
				fxCoderAddVariable(coder, 0, XS_CODE_NEW_CLOSURE, node->symbol, node->index);
				fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
				fxCoderAddIndex(coder, 0, XS_CODE_VAR_CLOSURE_1, node->index);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
			}
			node = node->nextDeclareNode;
		}
		fxCoderAddByte(coder, 1, XS_CODE_NULL);
		fxCoderAddByte(coder, 0, XS_CODE_WITH);
		node = self->firstDeclareNode;
		while (node) {
			if ((node->description->token == XS_TOKEN_DEFINE) || (node->description->token == XS_TOKEN_VAR))
				fxCoderAddIndex(coder, 0, XS_CODE_STORE_1, node->index);
			node = node->nextDeclareNode;
		}
		fxCoderAddByte(coder, -1, XS_CODE_POP);
		coder->environmentLevel++;
		node = self->firstDeclareNode;
		while (node) {
			if ((node->description->token != XS_TOKEN_DEFINE) && (node->description->token != XS_TOKEN_VAR)) {
				node->index = coder->scopeLevel++;
				if (node->symbol)
					fxCoderAddVariable(coder, 0, XS_CODE_NEW_CLOSURE, node->symbol, node->index);
				else
					fxCoderAddIndex(coder, 0, XS_CODE_NEW_TEMPORARY, node->index);
			}
			node = node->nextDeclareNode;
		}
		fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
		fxCoderAddByte(coder, 0, XS_CODE_WITH);
		node = self->firstDeclareNode;
		while (node) {
			if ((node->description->token != XS_TOKEN_DEFINE) && (node->description->token != XS_TOKEN_VAR)) {
				if (node->symbol)
					fxCoderAddIndex(coder, 0, XS_CODE_STORE_1, node->index);
			}
			node = node->nextDeclareNode;
		}
		fxCoderAddByte(coder, -1, XS_CODE_POP);
		coder->environmentLevel++;
	}
	else 
		fxScopeCodingBlock(self, coder);
}

void fxScopeCodingEval(txScope* self, txCoder* coder) 
{
	txProgramNode* programNode = (txProgramNode*)self->node;
	txDeclareNode* node;
	if (self->flags & mxStrictFlag) {
		if (programNode->scopeCount) {
			fxCoderAddIndex(coder, 0, XS_CODE_RESERVE_1, programNode->scopeCount);
			fxScopeCodingBlock(self, coder);
			node = self->firstDeclareNode;
			while (node) {
				if (node->description->token == XS_TOKEN_PRIVATE) {
					fxCoderAddSymbol(coder, 1, XS_CODE_EVAL_PRIVATE, node->symbol);
					fxCoderAddIndex(coder, 0, XS_CODE_CONST_CLOSURE_1, node->index);
					fxCoderAddByte(coder, -1, XS_CODE_POP);
				}
				node = node->nextDeclareNode;
			}
		}
	}
	else {
		txInteger count = 0;
		node = self->firstDeclareNode;
		while (node) {
			if ((node->description->token == XS_TOKEN_DEFINE) || (node->description->token == XS_TOKEN_VAR))
				count++;
			node = node->nextDeclareNode;
		}
		if (count) {
			fxCoderAddIndex(coder, 0, XS_CODE_RESERVE_1, count);
			node = self->firstDeclareNode;
			while (node) {
				if (node->description->token == XS_TOKEN_DEFINE) {
					node->index = coder->scopeLevel++;
					fxCoderAddVariable(coder, 0, XS_CODE_NEW_LOCAL, node->symbol, node->index);
					fxCoderAddByte(coder, 1, XS_CODE_NULL);
					fxCoderAddIndex(coder, 0, XS_CODE_VAR_LOCAL_1, node->index);
					fxCoderAddByte(coder, -1, XS_CODE_POP);
				}
				else if (node->description->token == XS_TOKEN_VAR) {
					node->index = coder->scopeLevel++;
					fxCoderAddVariable(coder, 0, XS_CODE_NEW_LOCAL, node->symbol, node->index);
					fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
					fxCoderAddIndex(coder, 0, XS_CODE_VAR_LOCAL_1, node->index);
					fxCoderAddByte(coder, -1, XS_CODE_POP);
				}
				node = node->nextDeclareNode;
			}
		}
		fxCoderAddByte(coder, 0, XS_CODE_EVAL_ENVIRONMENT);
		coder->scopeLevel = 0;
		if (programNode->scopeCount) {
			fxCoderAddIndex(coder, 0, XS_CODE_RESERVE_1, programNode->scopeCount);
			if (self->declareNodeCount) {
				node = self->firstDeclareNode;
				while (node) {
					if ((node->description->token != XS_TOKEN_DEFINE) && (node->description->token != XS_TOKEN_VAR)) {
						node->index = coder->scopeLevel++;
						if (node->flags & mxDeclareNodeClosureFlag) {
							fxCoderAddVariable(coder, 0, XS_CODE_NEW_CLOSURE, node->symbol, node->index);
						}
						else {
							fxCoderAddVariable(coder, 0, XS_CODE_NEW_LOCAL, node->symbol, node->index);
						}
					}
					node = node->nextDeclareNode;
				}
				if (self->flags & mxEvalFlag) {
					fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
					fxCoderAddByte(coder, 0, XS_CODE_WITH);
					node = self->firstDeclareNode;
					while (node) {
						if ((node->description->token != XS_TOKEN_DEFINE) && (node->description->token != XS_TOKEN_VAR))
							fxCoderAddIndex(coder, 0, XS_CODE_STORE_1, node->index);
						node = node->nextDeclareNode;
					}
					fxCoderAddByte(coder, -1, XS_CODE_POP);
					coder->environmentLevel++;
				}
			}
		}
	}
}

void fxScopeCodingParams(txScope* self, txCoder* coder) 
{
	txDeclareNode* node = self->firstDeclareNode;
	while (node) {
		txToken token = node->description->token;
		if ((token == XS_TOKEN_ARG) || (token == XS_TOKEN_VAR) || (token == XS_TOKEN_CONST)) {
			if (node->flags & mxDeclareNodeClosureFlag) {
				if (node->flags & mxDeclareNodeUseClosureFlag) {
					fxReportParserError(self->parser, node->line, "argument %s use closure", node->symbol->string);
				}
				node->index = coder->scopeLevel++;
				fxCoderAddVariable(coder, 0, XS_CODE_NEW_CLOSURE, node->symbol, node->index);
			}
			else {
				node->index = coder->scopeLevel++;
				fxCoderAddVariable(coder, 0, XS_CODE_NEW_LOCAL, node->symbol, node->index);
			}
		}
		node = node->nextDeclareNode;
	}
	if (self->flags & mxEvalFlag) {
		if (!(self->node->flags & mxStrictFlag)) {	
			fxCoderAddByte(coder, 1, XS_CODE_NULL);
			fxCoderAddByte(coder, 0, XS_CODE_WITH);
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			coder->environmentLevel++;
		}
		fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
		fxCoderAddByte(coder, 0, XS_CODE_WITH);
		node = self->firstDeclareNode;
		while (node) {
			txToken token = node->description->token;
			if ((token == XS_TOKEN_ARG) || (token == XS_TOKEN_VAR) || (token == XS_TOKEN_CONST))
				fxCoderAddIndex(coder, 0, XS_CODE_STORE_1, node->index);
			node = node->nextDeclareNode;
		}
		fxCoderAddByte(coder, -1, XS_CODE_POP);
		coder->environmentLevel++;
	}
}

void fxScopeCodingProgram(txScope* self, txCoder* coder) 
{
	txProgramNode* programNode = (txProgramNode*)self->node;
	txDeclareNode* node;
	txInteger count = 0;
	if (programNode->variableCount) {
		fxCoderAddIndex(coder, 0, XS_CODE_RESERVE_1, programNode->variableCount);
		node = self->firstDeclareNode;
		while (node) {
			if ((node->description->token != XS_TOKEN_DEFINE) && (node->description->token != XS_TOKEN_VAR)) {
				node->index = coder->scopeLevel++;
				fxCoderAddVariable(coder, 0, XS_CODE_NEW_CLOSURE, node->symbol, node->index);
			}
			node = node->nextDeclareNode;
		}
		count = coder->scopeLevel;
		node = self->firstDeclareNode;
		while (node) {
			if (node->description->token == XS_TOKEN_DEFINE) {
				// closure -> global property
				node->index = coder->scopeLevel++;
				fxCoderAddVariable(coder, 0, XS_CODE_NEW_LOCAL, node->symbol, node->index);
				fxCoderAddByte(coder, 1, XS_CODE_NULL);
				fxCoderAddIndex(coder, 0, XS_CODE_VAR_LOCAL_1, node->index);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
			}
			else if (node->description->token == XS_TOKEN_VAR) {
				// closure -> global property
				node->index = coder->scopeLevel++;
				fxCoderAddVariable(coder, 0, XS_CODE_NEW_LOCAL, node->symbol, node->index);
				fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
				fxCoderAddIndex(coder, 0, XS_CODE_VAR_LOCAL_1, node->index);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
			}
			node = node->nextDeclareNode;
		}
		fxCoderAddByte(coder, 0, XS_CODE_PROGRAM_ENVIRONMENT);
		coder->scopeLevel = count;
	}
	if (programNode->scopeCount > count)
		fxCoderAddIndex(coder, 0, XS_CODE_RESERVE_1, programNode->scopeCount - count);
}

void fxScopeCodeDefineNodes(txScope* self, txCoder* coder) 
{
	txDefineNode* node = self->firstDefineNode;
	while (node) {
		fxDefineNodeCode(node, coder);
		node = node->nextDefineNode;
	}
}

txInteger fxScopeCodeSpecifierNodes(txScope* self, txCoder* coder) 
{
	txDeclareNode* node = self->firstDeclareNode;
	txInteger count = 0;
	while (node) {
		if (node->flags & mxDeclareNodeUseClosureFlag) {
			txSpecifierNode* specifier = node->importSpecifier;
			txInteger index = 3;
			txBoolean flag = 0;
			if (node->symbol)
				fxCoderAddSymbol(coder, 1, XS_CODE_SYMBOL, node->symbol);
			else
				fxCoderAddByte(coder, 1, XS_CODE_NULL);
			if (specifier) {
				if (coder->parser->flags & mxDebugFlag) {
					fxCoderAddLine(coder, 0, XS_CODE_LINE, (txNode*)specifier);
				}
				fxStringNodeCode(specifier->from, coder);
				if (specifier->with)
					flag = 1;
				if (specifier->symbol)
					fxCoderAddSymbol(coder, 1, XS_CODE_SYMBOL, specifier->symbol);
				else
					fxCoderAddByte(coder, 1, XS_CODE_NULL);
			}
			else {
				fxCoderAddByte(coder, 1, XS_CODE_NULL);
				fxCoderAddByte(coder, 1, XS_CODE_NULL);
			}
			specifier = node->firstExportSpecifier;
			while (specifier) {
				if (specifier->asSymbol) {
					fxCoderAddSymbol(coder, 1, XS_CODE_SYMBOL, specifier->asSymbol);
					index++;
				}
				else if (specifier->symbol) {
					fxCoderAddSymbol(coder, 1, XS_CODE_SYMBOL, specifier->symbol);
					index++;
				}
				else {
					fxCoderAddByte(coder, 1, XS_CODE_NULL);
					index++;
				}
				specifier = specifier->nextSpecifier;
			}
			fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, index);
			if (flag)
				fxCoderAddByte(coder, 0 - index, XS_CODE_TRANSFER_JSON);
			else
				fxCoderAddByte(coder, 0 - index, XS_CODE_TRANSFER);
			count++;
		}
		node = node->nextDeclareNode;
	}
	return count;
}

void fxScopeCodeRefresh(txScope* self, txCoder* coder) 
{
	txDeclareNode* node = self->firstDeclareNode;
	while (node) {
		if (node->flags & mxDeclareNodeClosureFlag)
			fxCoderAddIndex(coder, 0, XS_CODE_REFRESH_CLOSURE_1, node->index);
		else
			fxCoderAddIndex(coder, 0, XS_CODE_REFRESH_LOCAL_1, node->index);
		node = node->nextDeclareNode;
	}
}

void fxScopeCodeReset(txScope* self, txCoder* coder) 
{
	txDeclareNode* node = self->firstDeclareNode;
	while (node) {
		if (node->flags & mxDeclareNodeClosureFlag)
			fxCoderAddIndex(coder, 0, XS_CODE_RESET_CLOSURE_1, node->index);
		else if (node->symbol)
			fxCoderAddIndex(coder, 0, XS_CODE_RESET_LOCAL_1, node->index);
		else {
			fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
			fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, node->index);
		}
		node = node->nextDeclareNode;
	}
}

void fxScopeCodeRetrieve(txScope* self, txCoder* coder) 
{
	txDeclareNode* node;
	txInteger count = 0;
	node = self->firstDeclareNode;
	while (node) {
		if ((node->flags & mxDeclareNodeUseClosureFlag) && node->symbol) {
			node->index = coder->scopeLevel++;
			count++;
		}
		node = node->nextDeclareNode;
	}
	if ((self->node->flags & mxArrowFlag) && ((self->node->flags & mxDefaultFlag) || (self->flags & mxEvalFlag))) {
		fxCoderAddIndex(coder, 0, XS_CODE_RETRIEVE_1, count);
		fxCoderAddByte(coder, 0, XS_CODE_RETRIEVE_TARGET);
		fxCoderAddByte(coder, 0, XS_CODE_RETRIEVE_THIS);
	}
	else if (count)
		fxCoderAddIndex(coder, 0, XS_CODE_RETRIEVE_1, count);
	self->closureNodeCount = count;
}

void fxScopeCodeStore(txScope* self, txCoder* coder) 
{
	txDeclareNode* node = self->firstDeclareNode;
	txUnsigned flags = self->flags & mxEvalFlag;
	while (node) {
		if (node->flags & mxDeclareNodeUseClosureFlag) {
			fxCoderAddIndex(coder, 0, XS_CODE_STORE_1, node->declaration->index);
			node->declaration->flags |= flags;
		}
		node = node->nextDeclareNode;
	}
	if ((self->node->flags & mxArrowFlag) && ((self->node->flags & mxDefaultFlag) || (self->flags & mxEvalFlag)))
		fxCoderAddByte(coder, 0, XS_CODE_STORE_ARROW);
	if (self->flags & mxEvalFlag)
		fxScopeCodeStoreAll(self->scope, coder);
}

void fxScopeCodeStoreAll(txScope* self, txCoder* coder) 
{
	txScope* scope = self;
	while (scope) {
		txDeclareNode* node;
		if (scope->token == XS_TOKEN_WITH)
			break;
		node = scope->firstDeclareNode;
		while (node) {
			if (node->flags & mxEvalFlag)
				node->flags &= ~mxEvalFlag;
			else if ((!(node->flags & mxDeclareNodeUseClosureFlag)) && node->declaration)
				fxCoderAddIndex(coder, 0, XS_CODE_STORE_1, node->declaration->index);
			node = node->nextDeclareNode;
		}
		if ((scope->token == XS_TOKEN_FUNCTION) || (scope->token == XS_TOKEN_MODULE))
			break;
		scope = scope->scope;
	}
}

void fxScopeCodeUsed(txScope* self, txCoder* coder, txUsingContext* context) 
{
	txTargetCode* normalTarget = fxCoderCreateTarget(coder);
	txTargetCode* uncatchTarget = fxCoderCreateTarget(coder);
	txTargetCode* finallyTarget = fxCoderCreateTarget(coder);
	txTargetCode* elseTarget = fxCoderCreateTarget(coder);
	txInteger selection;
	
	fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, normalTarget);
	fxCoderAdd(coder, 0, context->catchTarget);
	fxCoderAddByte(coder, 1, XS_CODE_EXCEPTION);
	fxCoderAddIndex(coder, 0, XS_CODE_PULL_LOCAL_1, context->exception);
	fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, 0);
	fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, context->selector);
	fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, finallyTarget);
	selection = 1;
	coder->firstBreakTarget = fxCoderFinalizeTargets(coder, coder->firstBreakTarget, context->selector, &selection, uncatchTarget);
	coder->firstContinueTarget = fxCoderFinalizeTargets(coder, coder->firstContinueTarget, context->selector, &selection, uncatchTarget);
	coder->returnTarget = fxCoderFinalizeTargets(coder, coder->returnTarget, context->selector, &selection, uncatchTarget);
	fxCoderAdd(coder, 0, normalTarget);
	fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, selection);
	fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, context->selector);
	fxCoderAdd(coder, 0, uncatchTarget);
	fxCoderAddByte(coder, 0, XS_CODE_UNCATCH);
	fxCoderAdd(coder, 0, finallyTarget);
	
	fxScopeCodeUsedReverse(self, coder, self->firstDeclareNode, context->exception, context->selector);
	
	fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, context->selector);
	fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, elseTarget);
	fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, context->exception);
	fxCoderAddByte(coder, -1, XS_CODE_THROW);
	fxCoderAdd(coder, 0, elseTarget);
	selection = 1;
	fxCoderJumpTargets(coder, coder->firstBreakTarget, context->selector, &selection);
	fxCoderJumpTargets(coder, coder->firstContinueTarget, context->selector, &selection);
	fxCoderJumpTargets(coder, coder->returnTarget, context->selector, &selection);
	fxCoderUnuseTemporaryVariables(coder, 2);
}

void fxScopeCodeUsedReverse(txScope* self, txCoder* coder, txDeclareNode* node, txInteger exception, txInteger selector) 
{
	/* xs_no_recursion: emit from the END of the declare-node list toward
	   the head (the original recursed to the end, then emitted on unwind,
	   so the last node emits first); iteration replaces recursion */
	txDeclareNode* tail;
	txDeclareNode** nodes;
	int count = 0, i = 0;
	for (tail = node; tail; tail = tail->nextDeclareNode)
		count++;
	nodes = (txDeclareNode**)fxNewParserChunk(coder->parser, (count ? count : 1) * sizeof(txDeclareNode*));
	for (tail = node; tail; tail = tail->nextDeclareNode)
		nodes[i++] = tail;
	while (count > 0) {
		txDeclareNode* current = nodes[--count];
		fxCheckParserStack(coder->parser, current->line);
		if (current->description->token == XS_TOKEN_USING) {
			txTargetCode* catchTarget = fxCoderCreateTarget(coder);
			txTargetCode* chainTarget = fxCoderCreateTarget(coder);
			txTargetCode* normalTarget = fxCoderCreateTarget(coder);
			
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, current->nextDeclareNode->index);
			fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
			fxCoderAddByte(coder, -1, XS_CODE_STRICT_EQUAL);
			fxCoderAddBranch(coder, -1, XS_CODE_BRANCH_IF_1, normalTarget);
			
			fxCoderAddBranch(coder, 0, XS_CODE_CATCH_1, catchTarget);
			
			fxCoderAddIndex(coder, 1, (current->flags & mxDeclareNodeClosureFlag) ? XS_CODE_GET_CLOSURE_1: XS_CODE_GET_LOCAL_1, current->index);
			fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_CHAIN_1, chainTarget);
			fxCoderAddIndex(coder, 1, XS_CODE_GET_LOCAL_1, current->nextDeclareNode->index);
			fxCoderAddByte(coder, 1, XS_CODE_CALL);
			fxCoderAddInteger(coder, -2, XS_CODE_RUN_1, 0);
			
			fxCoderAdd(coder, 0, chainTarget);
			if (current->flags & mxAwaitingFlag) {
				fxCoderAddByte(coder, 0, XS_CODE_AWAIT);
				fxCoderAddByte(coder, 0, XS_CODE_THROW_STATUS);
			}
			fxCoderAddByte(coder, -1, XS_CODE_POP);
			
			fxCoderAddByte(coder, 0, XS_CODE_UNCATCH);
			fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, normalTarget);
			fxCoderAdd(coder, 0, catchTarget);
			fxCoderAddIndex(coder, 1, XS_CODE_USED_1, selector);
			
			fxCoderAdd(coder, 0, normalTarget);
		}
	}
}

void fxScopeCodeUsing(txScope* self, txCoder* coder, txUsingContext* context) 
{
	if (self->token == XS_TOKEN_MODULE) {
		txDeclareNode* node = self->firstDeclareNode;
		while (node) {
			if (node->flags & mxDeclareNodeDisposableFlag) {
				node->index = coder->scopeLevel++;
				fxCoderAddIndex(coder, 0, XS_CODE_NEW_TEMPORARY, node->index);
			}
			node = node->nextDeclareNode;
		}
	}

	context->exception = fxCoderUseTemporaryVariable(coder);
	context->selector = fxCoderUseTemporaryVariable(coder);
	coder->firstBreakTarget = fxCoderAliasTargets(coder, coder->firstBreakTarget);
	coder->firstContinueTarget = fxCoderAliasTargets(coder, coder->firstContinueTarget);
	coder->returnTarget = fxCoderAliasTargets(coder, coder->returnTarget);
	context->catchTarget = fxCoderCreateTarget(coder);
	fxCoderAddBranch(coder, 0, XS_CODE_CATCH_1, context->catchTarget);
}

void fxScopeCodeUsingStatement(txScope* self, txCoder* coder, txNode* statement) 
{
	if (self->disposableNodeCount) {
		txUsingContext context;
		fxScopeCodeUsing(self, coder, &context);
		fxNodeDispatchCode(statement, coder);
		fxScopeCodeUsed(self, coder, &context);
	}
	else
		fxNodeDispatchCode(statement, coder);
}

/* xs_no_recursion: iterative walker call helpers. When the pump is
   already running (a stage body is executing) they push a frame and
   return; when called natively (tree dispatch tables, fxParserCode,
   fxScopeCodeUsingStatement) they drain synchronously, giving the
   original recursive-call contract. The extra running-flag save/restore
   makes nested native dispatch (fxScopeCodeUsingStatement inside a
   stage body) safe when the longjmp error path unwinds. */
static void fxNodeDispatchCodeAssignCall(txParser* parser, void* it, void* param, int kind, txFlag flag)
{
	txNodeWalkFrame* base;
	int running;
	if (parser->nodeWalkRunning) {
		fxNodeWalkPushCode(parser, it, param, kind, flag);
		return;
	}
	base = fxNodeWalkStackTop(parser);
	running = parser->nodeWalkRunning;
	parser->nodeWalkRunning = 1;
	fxNodeWalkPushCode(parser, it, param, kind, flag);
	fxNodeWalkDrainCode(parser, base);
	parser->nodeWalkRunning = running;
}

static void fxNodeDispatchCodeDeleteCall(txParser* parser, void* it, void* param, int kind)
{
	txNodeWalkFrame* base;
	int running;
	if (parser->nodeWalkRunning) {
		fxNodeWalkPushCode(parser, it, param, kind, 0);
		return;
	}
	base = fxNodeWalkStackTop(parser);
	running = parser->nodeWalkRunning;
	parser->nodeWalkRunning = 1;
	fxNodeWalkPushCode(parser, it, param, kind, 0);
	fxNodeWalkDrainCode(parser, base);
	parser->nodeWalkRunning = running;
}

static void fxNodeDispatchCodeReferenceCall(txParser* parser, void* it, void* param, int kind, txFlag flag)
{
	txNodeWalkFrame* base;
	int running;
	if (parser->nodeWalkRunning) {
		fxNodeWalkPushCode(parser, it, param, kind, flag);
		return;
	}
	base = fxNodeWalkStackTop(parser);
	running = parser->nodeWalkRunning;
	parser->nodeWalkRunning = 1;
	fxNodeWalkPushCode(parser, it, param, kind, flag);
	fxNodeWalkDrainCode(parser, base);
	parser->nodeWalkRunning = running;
}

static txFlag fxNodeDispatchCodeThisCall(txParser* parser, void* it, void* param, int kind, txFlag flag)
{
	txNodeWalkFrame* base;
	int running;
	if (parser->nodeWalkRunning) {
		fxNodeWalkPushCode(parser, it, param, kind, flag);
		/* xs_no_recursion (R7/B2): the This result of a child machine frame
		   is read by the parent after its dispatch resumes. */
		parser->codeThisResult = 0;
		return 0;
	}
	base = fxNodeWalkStackTop(parser);
	running = parser->nodeWalkRunning;
	parser->nodeWalkRunning = 1;
	fxNodeWalkPushCode(parser, it, param, kind, flag);
	fxNodeWalkDrainCode(parser, base);
	parser->nodeWalkRunning = running;
	return parser->codeThisResult;
}

static void fxNodeDispatchCodeCall(txParser* parser, void* it, void* param, int kind)
{
	txNodeWalkFrame* base;
	int running;
	txCoder* coder = param;
	if (parser->nodeWalkRunning) {
		fxNodeWalkPushCode(parser, it, param, kind, 0);
		return;
	}
	base = fxNodeWalkStackTop(parser);
	running = parser->nodeWalkRunning;
	parser->nodeWalkRunning = 1;
	fxNodeWalkPushCode(parser, it, param, kind, 0);
	fxNodeWalkDrainCode(parser, base);
	parser->nodeWalkRunning = running;
	(void)coder;
}

/* xs_no_recursion: host-only code trace (opt-in; never compiled for device) */
static void fxNodeDispatchCode(void* it, void* param)
{
	txNode* self = it;
	txCoder* coder = param;
	int kind;
	fxCheckParserStack(coder->parser, self->line);
	if (self->line != kNoLine)
		fxCoderAddLine(coder, 0, XS_CODE_LINE, self); 
	kind = fxCodeStepKindFor((void*)(*self->description->dispatch->code));
	if (kind) {
		if (fxNodeWalkRunningCheck(coder->parser)) {
			/* xs_no_recursion (R7): called from fxNodeCodeBody while a pump
			   is draining (a machine child dispatching its own subtree).
			   Converted child: push its frame (the drain visits it). Native
			   child: run it inline but with the running flag CLEARED so its
			   native descendants run natively too — otherwise stock code
			   continuing after a dispatch (which the pump cannot suspend)
			   would see its later dispatches deferred to after its epilogue.
			   The drain's stack pointer is preserved by the Call wrapper. */
			if (kind == WC_THROW || kind == WC_UNARY || kind == WC_PRIVATE_IDENTIFIER
				|| kind == WC_MEMBER || kind == WC_MEMBER_ASSIGN || kind == WC_MEMBER_DELETE || kind == WC_MEMBER_REFERENCE
				|| kind == WC_ASSIGN || kind == WC_BINARY || kind == WC_QUESTION
				|| kind == WC_AND || kind == WC_OR || kind == WC_COALESCE || kind == WC_COMPOUND
				|| kind == WC_POSTFIX || kind == WC_EXPRESSIONS
				|| kind == WC_EXPRESSIONS_THIS || kind == WC_EXPRESSIONS_DELETE
				|| kind == WC_CALL || kind == WC_NEW || kind == WC_CHAIN || kind == WC_OPTION
				|| kind == WC_MEMBER_AT || kind == WC_PRIVATE_MEMBER
				|| kind == WC_TEMPLATE || kind == WC_SPREAD
				|| kind == WC_BLOCK || kind == WC_BODY || kind == WC_STATEMENTS
				|| kind == WC_STATEMENT || kind == WC_LABEL || kind == WC_WHILE
				|| kind == WC_DO || kind == WC_IF || kind == WC_WITH || kind == WC_RETURN
				|| kind == WC_FOR || kind == WC_FORINFOROF || kind == WC_SWITCH
				|| kind == WC_CATCH || kind == WC_TRY || kind == WC_AWAIT
				|| kind == WC_YIELD || kind == WC_DELETE
				|| kind == WC_DECLARE || kind == WC_DECLARE_ASSIGN || kind == WC_DECLARE_REFERENCE
				|| kind == WC_DEFINE || kind == WC_FIELD
				|| kind == WC_PARAMS || kind == WC_OBJECT || kind == WC_ARRAY || kind == WC_FUNCTION
				|| kind == WC_CLASS
				|| kind == WC_INCLUDE || kind == WC_IMPORT_CALL || kind == WC_MODULE || kind == WC_PROGRAM
				|| kind == WC_BINDING || kind == WC_BINDING_ASSIGN || kind == WC_BINDING_REFERENCE
				|| kind == WC_COMPOUND_NAME || kind == WC_MEMBER_AT_ASSIGN || kind == WC_MEMBER_AT_THIS
				|| kind == WC_MEMBER_THIS || kind == WC_OBJECT_BINDING_ASSIGN || kind == WC_PARAMS_BINDING
				|| kind == WC_PRIVATE_MEMBER_THIS || kind == WC_REGEXP || kind == WC_SUPER
				|| kind == WC_DELEGATE || kind == WC_ARRAY_BINDING_ASSIGN)
				fxNodeDispatchCodeCall(coder->parser, it, param, kind);		else
			fxNodeWalkCallCode(coder->parser, it, param, WC_NATIVE, 0, 0, 0);
		}
		else
			fxNodeDispatchCodeCall(coder->parser, it, param, kind);
	}
	else {
		if (fxNodeWalkRunningCheck(coder->parser))
			fxNodeWalkCallCode(coder->parser, it, param, WC_NATIVE, 0, 0, 0);
		else
			(*self->description->dispatch->code)(it, param);
	}
}

void fxNodeDispatchCodeAssign(void* it, void* param, txFlag flag)
{
	txNode* self = it;
	txCoder* coder = param;
	int kind;
	fxCheckParserStack(coder->parser, self->line);
	if (self->line != kNoLine)
		fxCoderAddLine(coder, 0, XS_CODE_LINE, self); 
	kind = fxCodeStepKindFor((void*)(*self->description->dispatch->codeAssign));
	if (kind)
		fxNodeDispatchCodeAssignCall(coder->parser, it, param, kind, flag);
	else if (fxNodeWalkRunningCheck(coder->parser))
		/* xs_no_recursion (R7/B2): unconverted Assign variant reached from a
		   converted frame. Wrap it: the variant's own plain-code dispatches
		   run with running cleared (WC_NATIVE contract), and any epilogue
		   this frame still emits lands after the variant completes. */
		fxNodeWalkCallCode(coder->parser, it, param, WC_NASSIGN, flag, 0, 0);
	else
		(*self->description->dispatch->codeAssign)(self, param, flag);
}

void fxNodeDispatchCodeDelete(void* it, void* param)
{
	txNode* self = it;
	txCoder* coder = param;
	int kind;
	fxCheckParserStack(coder->parser, self->line);
	if (self->line != kNoLine)
		fxCoderAddLine(coder, 0, XS_CODE_LINE, self); 
	kind = fxCodeStepKindFor((void*)(*self->description->dispatch->codeDelete));
	if (kind)
		fxNodeDispatchCodeDeleteCall(coder->parser, it, param, kind);
	else
		(*self->description->dispatch->codeDelete)(self, param);
}

void fxNodeDispatchCodeReference(void* it, void* param, txFlag flag)
{
	txNode* self = it;
	txCoder* coder = param;
	int kind;
	fxCheckParserStack(coder->parser, self->line);
	if (self->line != kNoLine)
		fxCoderAddLine(coder, 0, XS_CODE_LINE, self); 
	kind = fxCodeStepKindFor((void*)(*self->description->dispatch->codeReference));
	if (kind)
		fxNodeDispatchCodeReferenceCall(coder->parser, it, param, kind, flag);
	else if (fxNodeWalkRunningCheck(coder->parser))
		fxNodeWalkCallCode(coder->parser, it, param, WC_NREF, flag, 0, 0);	/* R7/B2 */
	else
		(*self->description->dispatch->codeReference)(self, param, flag);
}

txFlag fxNodeDispatchCodeThis(void* it, void* param, txFlag flag) 
{
	txNode* self = it;
	txCoder* coder = param;
	int kind;
	fxCheckParserStack(coder->parser, self->line);
	if (self->line != kNoLine)
		fxCoderAddLine(coder, 0, XS_CODE_LINE, self); 
	kind = fxCodeStepKindFor((void*)(*self->description->dispatch->codeThis));
	if (kind)
		return fxNodeDispatchCodeThisCall(coder->parser, it, param, kind, flag);
	if (fxNodeWalkRunningCheck(coder->parser)) {
		/* xs_no_recursion (R7/B2): result staged in parser->codeThisResult
		   by the WC_NTHIS body (native variants write it there). */
		fxNodeWalkCallCode(coder->parser, it, param, WC_NTHIS, flag, 0, 0);
		return (txFlag)coder->parser->codeThisResult;
	}
	return (*self->description->dispatch->codeThis)(self, param, flag);
}

void fxNodeCode(void* it, void* param) 
{
	txNode* self = it;
	txCoder* coder = param;
	fxReportParserError(coder->parser, self->line, "no value");
	fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
}

void fxNodeCodeAssign(void* it, void* param, txFlag flag) 
{
	txNode* self = it;
	txCoder* coder = param;
	fxReportParserError(coder->parser, self->line, "no reference");
}

void fxNodeCodeDelete(void* it, void* param) 
{
	fxNodeDispatchCode(it, param);
	fxCoderAddByte(param, -1, XS_CODE_POP);
	fxCoderAddByte(param, 1, XS_CODE_TRUE);
}

void fxNodeCodeReference(void* it, void* param, txFlag flag) 
{
}

txFlag fxNodeCodeName(txNode* value)
{
	txToken token = value->description->token;
	if (token == XS_TOKEN_EXPRESSIONS) {
		value = ((txExpressionsNode*)value)->items->first;
		if (value->next)
			return 0;
		token = value->description->token;
	}
	if (token == XS_TOKEN_CLASS) {
		txClassNode* node = (txClassNode*)value;
		if (node->symbol)
			return 0;
	}
	else if ((token == XS_TOKEN_FUNCTION) || (token == XS_TOKEN_GENERATOR) || (token == XS_TOKEN_HOST)) {
		txFunctionNode* node = (txFunctionNode*)value;
		if (node->symbol)
			return 0;
	}
	else
		return 0;
	return 1;
}

txFlag fxNodeCodeThis(void* it, void* param, txFlag flag) 
{
	fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
	fxNodeDispatchCode(it, param);
	return 1;
}

void fxAccessNodeCode(void* it, void* param) 
{
	txAccessNode* self = it;
	txDeclareNode* declaration = self->declaration;
	if (!declaration) {
		fxAccessNodeCodeReference(it, param, 0);
		fxCoderAddSymbol(param, 0, XS_CODE_GET_VARIABLE, self->symbol);
	}
	else
		fxCoderAddIndex(param, 1, (declaration->flags & mxDeclareNodeClosureFlag) ? XS_CODE_GET_CLOSURE_1 : XS_CODE_GET_LOCAL_1, declaration->index);
}

void fxAccessNodeCodeAssign(void* it, void* param, txFlag flag) 
{
	txAccessNode* self = it;
	txDeclareNode* declaration = self->declaration;
	if (!declaration)
		fxCoderAddSymbol(param, -1, XS_CODE_SET_VARIABLE, self->symbol);
	else
		fxCoderAddIndex(param, 0, (declaration->flags & mxDeclareNodeClosureFlag) ? XS_CODE_SET_CLOSURE_1 : XS_CODE_SET_LOCAL_1, declaration->index);
}

void fxAccessNodeCodeDelete(void* it, void* param) 
{
	txAccessNode* self = it;
	txCoder* coder = param;
	txDeclareNode* declaration = self->declaration;
	if (self->flags & mxStrictFlag)
		fxReportParserError(coder->parser, self->line, "delete identifier (strict code)");
	if (!declaration) {
		fxAccessNodeCodeReference(it, param, 0);
		fxCoderAddSymbol(param, 0, XS_CODE_DELETE_PROPERTY, self->symbol);
	}
	else
		fxCoderAddByte(param, 1, XS_CODE_FALSE);
}

void fxAccessNodeCodeReference(void* it, void* param, txFlag flag) 
{
	txAccessNode* self = it;
	txCoder* coder = param;
	txDeclareNode* declaration = self->declaration;
	if (!declaration) {
		if (coder->evalFlag)
			fxCoderAddSymbol(param, 1, XS_CODE_EVAL_REFERENCE, self->symbol);
		else
			fxCoderAddSymbol(param, 1, XS_CODE_PROGRAM_REFERENCE, self->symbol);
	}
}

txFlag fxAccessNodeCodeThis(void* it, void* param, txFlag flag) 
{
	txAccessNode* self = it;
	txDeclareNode* declaration = self->declaration;
	if (!flag)
		fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
	if (!declaration) {
		fxAccessNodeCodeReference(it, param, 0);
		if (flag)
			fxCoderAddByte(param, 1, XS_CODE_DUB);
		fxCoderAddSymbol(param, 0, XS_CODE_GET_THIS_VARIABLE, self->symbol);
	}
	else {
		fxCoderAddIndex(param, 1, (declaration->flags & mxDeclareNodeClosureFlag) ? XS_CODE_GET_CLOSURE_1 : XS_CODE_GET_LOCAL_1, declaration->index);
		flag = 0;
	}
	return flag;
}

void fxAndExpressionNodeCode(void* it, void* param) 
{
	txBinaryExpressionNode* self = it;
	txTargetCode* endTarget = fxCoderCreateTarget(param);
	self->right->flags |= (self->flags & mxTailRecursionFlag);
	fxNodeDispatchCode(self->left, param);
	fxCoderAddByte(param, 1, XS_CODE_DUB);
	fxCoderAddBranch(param, -1, XS_CODE_BRANCH_ELSE_1, endTarget);
	fxCoderAddByte(param, -1, XS_CODE_POP);
	fxNodeDispatchCode(self->right, param);
	fxCoderAdd(param, 0, endTarget);
}

void fxArgumentsNodeCode(void* it, void* param) 
{
	fxCoderAddIndex(param, 1, XS_CODE_ARGUMENTS, 0);
}

void fxArrayNodeCode(void* it, void* param) 
{
	txArrayNode* self = it;
	txCoder* coder = param;
	txInteger array = fxCoderUseTemporaryVariable(param);
	fxCoderAddByte(param, 1, XS_CODE_ARRAY);
	fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, array);
	if (self->items) {
		txNode* item = self->items->first;
		if (self->flags & mxSpreadFlag) {
			txInteger counter = fxCoderUseTemporaryVariable(param);
			fxCoderAddInteger(param, 1, XS_CODE_INTEGER_1, 0);
			fxCoderAddIndex(param, -1, XS_CODE_PULL_LOCAL_1, counter);
			while (item) {
				if (item->description->token == XS_TOKEN_SPREAD) {
					txInteger iterator = fxCoderUseTemporaryVariable(param);
					txInteger result = fxCoderUseTemporaryVariable(param);
					txTargetCode* nextTarget = fxCoderCreateTarget(param);
					txTargetCode* doneTarget = fxCoderCreateTarget(param);
					fxNodeDispatchCode(((txSpreadNode*)item)->expression, param);
					fxCoderAddByte(param, 0, XS_CODE_FOR_OF);
					fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, iterator);
					fxCoderAddByte(param, -1, XS_CODE_POP);
					fxCoderAdd(param, 0, nextTarget);
					fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, iterator);
					fxCoderAddByte(param, 1, XS_CODE_DUB);
					fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->nextSymbol);
					fxCoderAddByte(param, 1, XS_CODE_CALL);
					fxCoderAddInteger(param, -2, XS_CODE_RUN_1, 0);
					fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, result);
					fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->doneSymbol);
					fxCoderAddBranch(param, -1, XS_CODE_BRANCH_IF_1, doneTarget);
					fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, array);
					fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, counter);
					fxCoderAddByte(param, 0, XS_CODE_AT);
					fxCoderAddIndex(param, 0, XS_CODE_GET_LOCAL_1, result);
					fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->valueSymbol);
					fxCoderAddByte(param, -2, XS_CODE_SET_PROPERTY_AT);
					fxCoderAddByte(param, -1, XS_CODE_POP);
					fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, counter);
					fxCoderAddByte(param, 0, XS_CODE_INCREMENT);
					fxCoderAddIndex(param, -1, XS_CODE_PULL_LOCAL_1, counter);
					fxCoderAddBranch(param, 0, XS_CODE_BRANCH_1, nextTarget);
					fxCoderAdd(param, 1, doneTarget);
					fxCoderUnuseTemporaryVariables(param, 2);
				}
				else {
					if (item->description->token != XS_TOKEN_ELISION) {
						fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, array);
						fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, counter);
						fxCoderAddByte(param, 0, XS_CODE_AT);
						fxNodeDispatchCode(item, param);
						fxCoderAddByte(param, -2, XS_CODE_SET_PROPERTY_AT);
						fxCoderAddByte(param, -1, XS_CODE_POP);
						fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, counter);
						fxCoderAddByte(param, 0, XS_CODE_INCREMENT);
						fxCoderAddIndex(param, -1, XS_CODE_PULL_LOCAL_1, counter);
					}
					else {
						fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, array);
						fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, counter);
						fxCoderAddByte(param, 0, XS_CODE_INCREMENT);
						fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, counter);
						fxCoderAddSymbol(param, -1, XS_CODE_SET_PROPERTY, coder->parser->lengthSymbol);
						fxCoderAddByte(param, -1, XS_CODE_POP);
					}
				}
				item = item->next;
			}
			fxCoderUnuseTemporaryVariables(param, 1);
		}
		else {
			txInteger index = 0;
			txInteger count = self->items->length;
			fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, array);
			fxCoderAddInteger(param, 1, XS_CODE_INTEGER_1, count);
			fxCoderAddSymbol(param, -1, XS_CODE_SET_PROPERTY, coder->parser->lengthSymbol);
			fxCoderAddByte(param, -1, XS_CODE_POP);
			while (item) {
				if (item->description->token == XS_TOKEN_ELISION)
					break;
				item = item->next;
			}
// 			if (!item) {
// 				fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, array);
// 				fxCoderAddByte(param, 1, XS_CODE_DUB);
// 				fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->fillSymbol);
// 				fxCoderAddByte(param, 1, XS_CODE_CALL);
// 				fxCoderAddInteger(param, -2, XS_CODE_RUN_1, 0);
// 				fxCoderAddByte(param, -1, XS_CODE_POP);
// 			}
			item = self->items->first;
			while (item) {
				if (item->description->token != XS_TOKEN_ELISION) {
					fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, array);
					fxCoderAddInteger(param, 1, XS_CODE_INTEGER_1, index);
					fxCoderAddByte(param, 0, XS_CODE_AT);
					fxNodeDispatchCode(item, param);
					fxCoderAddByte(param, -3, XS_CODE_NEW_PROPERTY_AT);
					fxCoderAddInteger(param, 0, XS_CODE_INTEGER_1, 0);
				}
				item = item->next;
				index++;
			}
		}
	}
	fxCoderUnuseTemporaryVariables(param, 1);
}

void fxArrayBindingNodeCode(void* it, void* param)
{
	txArrayBindingNode* self = it;
	txCoder* coder = param;
	fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
	fxArrayBindingNodeCodeAssign(self, param, 0);
}

void fxArrayBindingNodeCodeAssign(void* it, void* param, txFlag flag) 
{
	txArrayBindingNode* self = it;
	txCoder* coder = param;
	txNode* item = self->items->first;
	txInteger iterator;
	txInteger next;
	txInteger done;
	txInteger rest;
	txInteger result;
	txInteger selector;
	txInteger selection;
	txTargetCode* catchTarget;
	txTargetCode* normalTarget;
	txTargetCode* finallyTarget;
	
	txTargetCode* returnTarget;
	txTargetCode* stepTarget;
	txTargetCode* doneTarget;
	txTargetCode* nextTarget;
	
	iterator = fxCoderUseTemporaryVariable(param);
	next = fxCoderUseTemporaryVariable(param);
	done = fxCoderUseTemporaryVariable(param);
	selector = fxCoderUseTemporaryVariable(param);
	rest = fxCoderUseTemporaryVariable(param);
	result = fxCoderUseTemporaryVariable(param);
	
	coder->returnTarget = fxCoderAliasTargets(param, coder->returnTarget);
	catchTarget = fxCoderCreateTarget(param);
	normalTarget = fxCoderCreateTarget(param);
	finallyTarget = fxCoderCreateTarget(param);
	
	fxCoderAddByte(param, 1, XS_CODE_DUB);
	fxCoderAddByte(param, 0, XS_CODE_FOR_OF);
	fxCoderAddIndex(param, -1, XS_CODE_PULL_LOCAL_1, iterator);
	fxCoderAddByte(param, 1, XS_CODE_FALSE);
	fxCoderAddIndex(param, -1, XS_CODE_PULL_LOCAL_1, done);
	fxCoderAddInteger(param, 1, XS_CODE_INTEGER_1, 0);
	fxCoderAddIndex(param, -1, XS_CODE_PULL_LOCAL_1, selector);
	fxCoderAddBranch(param, 0, XS_CODE_CATCH_1, catchTarget);
	
	if (item) {
		fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, iterator);
		fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->nextSymbol);
		fxCoderAddIndex(param, 0, XS_CODE_PULL_LOCAL_1, next);
	
		while (item && (item->description->token != XS_TOKEN_REST_BINDING)) {
			stepTarget = fxCoderCreateTarget(param);
			
			if (item->description->token == XS_TOKEN_SKIP_BINDING) {
				fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, done);
				fxCoderAddBranch(param, -1, XS_CODE_BRANCH_IF_1, stepTarget);
				fxCoderAddByte(param, 1, XS_CODE_TRUE);
				fxCoderAddIndex(param, -1, XS_CODE_PULL_LOCAL_1, done);
				fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, iterator);
				fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, next);
				fxCoderAddByte(param, 1, XS_CODE_CALL);
				fxCoderAddInteger(param, -2, XS_CODE_RUN_1, 0);
				fxCoderAddByte(param, 0, XS_CODE_CHECK_INSTANCE);
				fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->doneSymbol);
				fxCoderAddIndex(param, 0, XS_CODE_PULL_LOCAL_1, done);
				fxCoderAdd(param, 1, stepTarget);
			}
			else {
				doneTarget = fxCoderCreateTarget(param);
				nextTarget = fxCoderCreateTarget(param);
				fxNodeDispatchCodeReference(item, param, 1);
				
				fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, done);
				fxCoderAddBranch(param, -1, XS_CODE_BRANCH_IF_1, stepTarget);
				fxCoderAddByte(param, 1, XS_CODE_TRUE);
				fxCoderAddIndex(param, -1, XS_CODE_PULL_LOCAL_1, done);
				fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, iterator);
				fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, next);
				fxCoderAddByte(param, 1, XS_CODE_CALL);
				fxCoderAddInteger(param, -2, XS_CODE_RUN_1, 0);
				fxCoderAddByte(param, 0, XS_CODE_CHECK_INSTANCE);
				fxCoderAddByte(param, 1, XS_CODE_DUB);
				fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->doneSymbol);
				fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, done);
				fxCoderAddBranch(param, -1, XS_CODE_BRANCH_IF_1, doneTarget);
				fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->valueSymbol);
				fxCoderAddBranch(param, 0, XS_CODE_BRANCH_1, nextTarget);
				fxCoderAdd(param, 1, doneTarget);
				fxCoderAddByte(param, -1, XS_CODE_POP);
				fxCoderAdd(param, 1, stepTarget);
				fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
				fxCoderAdd(param, 1, nextTarget);
				fxNodeDispatchCodeAssign(item, param, 1);
				fxCoderAddByte(param, -1, XS_CODE_POP);
			}
			item = item->next;
		}
		if (item) {
			nextTarget = fxCoderCreateTarget(param);
			doneTarget = fxCoderCreateTarget(param);
		
			fxNodeDispatchCodeReference(((txRestBindingNode*)item)->binding, param, 1);
			fxCoderAddByte(param, 1, XS_CODE_ARRAY);
			fxCoderAddIndex(param, -1, XS_CODE_PULL_LOCAL_1, rest);
			
			fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, done);
			fxCoderAddBranch(param, -1, XS_CODE_BRANCH_IF_1, doneTarget);

			fxCoderAdd(param, 0, nextTarget);
			fxCoderAddByte(param, 1, XS_CODE_TRUE);
			fxCoderAddIndex(param, -1, XS_CODE_PULL_LOCAL_1, done);
			fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, iterator);
			fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, next);
			fxCoderAddByte(param, 1, XS_CODE_CALL);
			fxCoderAddInteger(param, -2, XS_CODE_RUN_1, 0);
			fxCoderAddByte(param, 0, XS_CODE_CHECK_INSTANCE);
			fxCoderAddIndex(param, 1, XS_CODE_SET_LOCAL_1, result);
			fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->doneSymbol);
			fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, done);
			
			fxCoderAddBranch(param, -1, XS_CODE_BRANCH_IF_1, doneTarget);
		
			fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, rest);
			fxCoderAddByte(param, 1, XS_CODE_DUB);
			fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->lengthSymbol);
			fxCoderAddByte(param, 0, XS_CODE_AT);
			fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, result);
			fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->valueSymbol);
			fxCoderAddByte(param, -2, XS_CODE_SET_PROPERTY_AT);
			fxCoderAddByte(param, -1, XS_CODE_POP);
	
			fxCoderAddBranch(param, 0, XS_CODE_BRANCH_1, nextTarget);
			fxCoderAdd(param, 1, doneTarget);
		
			fxCoderAddIndex(param, 0, XS_CODE_GET_LOCAL_1, rest);
			fxNodeDispatchCodeAssign(((txRestBindingNode*)item)->binding, param, 1);
			fxCoderAddByte(param, -1, XS_CODE_POP);
		}
	
	}
	fxCoderAddBranch(param, 0, XS_CODE_BRANCH_1, normalTarget);
	
	selection = 1;
	coder->returnTarget = fxCoderFinalizeTargets(param, coder->returnTarget, selector, &selection, finallyTarget);
	fxCoderAdd(param, 0, normalTarget);
	fxCoderAddInteger(param, 1, XS_CODE_INTEGER_1, selection);
	fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, selector);
	fxCoderAddByte(param, -1, XS_CODE_POP);
	fxCoderAdd(param, 0, finallyTarget);
	fxCoderAddByte(param, 0, XS_CODE_UNCATCH);
	fxCoderAdd(param, 0, catchTarget);
	
	nextTarget = fxCoderCreateTarget(param);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, selector);
	fxCoderAddBranch(param, -1, XS_CODE_BRANCH_IF_1, nextTarget);
	fxCoderAddByte(param, 1, XS_CODE_EXCEPTION);
	fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, result);
	fxCoderAddByte(param, -1, XS_CODE_POP);
	catchTarget = fxCoderCreateTarget(param);
	fxCoderAddBranch(param, 0, XS_CODE_CATCH_1, catchTarget);
	fxCoderAdd(param, 0, nextTarget);
	
	doneTarget = fxCoderCreateTarget(param);
	returnTarget = fxCoderCreateTarget(param);
 	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, done);
 	fxCoderAddBranch(param, -1, XS_CODE_BRANCH_IF_1, doneTarget);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, iterator);
	fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->returnSymbol);
	fxCoderAddBranch(param, 0, XS_CODE_BRANCH_CHAIN_1, returnTarget);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, iterator);
	fxCoderAddByte(param, 0, XS_CODE_SWAP);
	fxCoderAddByte(param, 1, XS_CODE_CALL);
	fxCoderAddInteger(param, -2, XS_CODE_RUN_1, 0);
	fxCoderAddByte(param, 0, XS_CODE_CHECK_INSTANCE);
	fxCoderAdd(param, 0, returnTarget);
	fxCoderAddByte(param, -1, XS_CODE_POP);
	fxCoderAdd(param, 0, doneTarget);
	
	nextTarget = fxCoderCreateTarget(param);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, selector);
	fxCoderAddBranch(param, -1, XS_CODE_BRANCH_IF_1, nextTarget);
	fxCoderAddByte(param, 0, XS_CODE_UNCATCH);
	fxCoderAdd(param, 0, catchTarget);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, result);
	fxCoderAddByte(param, -1, XS_CODE_THROW);
	fxCoderAdd(param, 0, nextTarget);

	selection = 1;
	fxCoderJumpTargets(param, coder->returnTarget, selector, &selection);

	fxCoderUnuseTemporaryVariables(param, 6);
}

void fxAssignNodeCode(void* it, void* param) 
{
	txAssignNode* self = it;
	fxNodeDispatchCodeReference(self->reference, param, 1);
	fxNodeDispatchCode(self->value, param);
	fxNodeDispatchCodeAssign(self->reference, param, 1);
}

void fxAwaitNodeCode(void* it, void* param)
{
	txStatementNode* self = it;
	txCoder* coder = param;
	txTargetCode* target = fxCoderCreateTarget(coder);
	fxNodeDispatchCode(self->expression, param);
	fxCoderAddByte(param, 0, XS_CODE_AWAIT);
	fxCoderAddBranch(coder, 1, XS_CODE_BRANCH_STATUS_1, target);
	fxCoderAddByte(param, -1, XS_CODE_SET_RESULT);
	fxCoderAdjustEnvironment(coder, coder->returnTarget);
	fxCoderAdjustScope(coder, coder->returnTarget);
	fxCoderAddBranch(param, 0, XS_CODE_BRANCH_1, coder->returnTarget);
	fxCoderAdd(coder, 0, target);
}

void fxBigIntNodeCode(void* it, void* param) 
{
	txBigIntNode* self = it;
	fxCoderAddBigInt(param, 1, XS_CODE_BIGINT_1, &self->value);
}

void fxBinaryExpressionNodeCode(void* it, void* param) 
{
	txBinaryExpressionNode* self = it;
	fxNodeDispatchCode(self->left, param);
	fxNodeDispatchCode(self->right, param);
	fxCoderAddByte(param, -1, self->description->code);
}

void fxBindingNodeCode(void* it, void* param) 
{
	txBindingNode* self = it;
	txCoder* coder = param;
	
	if (self->target->description->token == XS_TOKEN_ACCESS) {
		fxReportParserError(coder->parser, self->line, "invalid initializer");
		fxNodeDispatchCode(self->initializer, param);
		return;
	}
	
	fxNodeDispatchCodeReference(self->target, param, 0);
	fxNodeDispatchCode(self->initializer, param);
	fxNodeDispatchCodeAssign(self->target, param, 0);
	fxCoderAddByte(coder, -1, XS_CODE_POP);
}

void fxBindingNodeCodeAssign(void* it, void* param, txFlag flag) 
{
	txBindingNode* self = it;
	txTargetCode* target = fxCoderCreateTarget(param);
	fxCoderAddByte(param, 1, XS_CODE_DUB);
	fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
	fxCoderAddByte(param, -1, XS_CODE_STRICT_NOT_EQUAL);
	fxCoderAddBranch(param, -1, XS_CODE_BRANCH_IF_1, target);
	fxCoderAddByte(param, -1, XS_CODE_POP);
	fxNodeDispatchCode(self->initializer, param);
	fxCoderAdd(param, 0, target);
	fxNodeDispatchCodeAssign(self->target, param, flag);
}

void fxBindingNodeCodeReference(void* it, void* param, txFlag flag) 
{
	txBindingNode* self = it;
	fxNodeDispatchCodeReference(self->target, param, flag);
}

void fxBlockNodeCode(void* it, void* param) 
{
	txBlockNode* self = it;
	fxScopeCodingBlock(self->scope, param);
	fxScopeCodeDefineNodes(self->scope, param);
	fxScopeCodeUsingStatement(self->scope, param, self->statement);
	fxScopeCoded(self->scope, param);
}

void fxBodyNodeCode(void* it, void* param) 
{
	txBlockNode* self = it;
	txCoder* coder = param;
	txBoolean evalFlag = coder->evalFlag;
	if ((self->flags & mxEvalFlag) && !(self->flags & mxStrictFlag))
		coder->evalFlag = 1;
	fxScopeCodingBody(self->scope, param);
	fxScopeCodeDefineNodes(self->scope, param);
	fxScopeCodeUsingStatement(self->scope, param, self->statement);
	fxScopeCodedBody(self->scope, param);
	if ((self->flags & mxEvalFlag) && !(self->flags & mxStrictFlag))
		coder->evalFlag = evalFlag;
}

void fxBreakContinueNodeCode(void* it, void* param) 
{
	txBreakContinueNode* self = it;
	txCoder* coder = param;
	txTargetCode* target = (self->description->token == XS_TOKEN_BREAK) ? coder->firstBreakTarget : coder->firstContinueTarget;
	while (target) {
		txLabelNode* label = target->label;
		while (label) {
			if (label->symbol == self->symbol) {
				fxCoderAdjustEnvironment(coder, target);
				fxCoderAdjustScope(coder, target);
				fxCoderAddBranch(param, 0, XS_CODE_BRANCH_1, target);
				return;
			}
			label = label->nextLabel;
		}
		target = target->nextTarget;
	}
	if (self->description->token == XS_TOKEN_BREAK)
		fxReportParserError(coder->parser, self->line, "invalid break");
	else
		fxReportParserError(coder->parser, self->line, "invalid continue");
}

void fxCallNodeCode(void* it, void* param) 
{
	txCallNewNode* self = it;
	fxNodeDispatchCodeThis(self->reference, param, 0);
	fxCoderAddByte(param, 1, XS_CODE_CALL);
	self->params->flags |= self->flags & mxTailRecursionFlag;
	fxNodeDispatchCode(self->params, param);
}

void fxCatchNodeCode(void* it, void* param) 
{
	txCatchNode* self = it;
	if (self->parameter) {
		fxScopeCodingBlock(self->scope, param);
		fxNodeDispatchCodeReference(self->parameter, param, 0);
		fxCoderAddByte(param, 1, XS_CODE_EXCEPTION);
		fxNodeDispatchCodeAssign(self->parameter, param, 0);
		fxCoderAddByte(param, -1, XS_CODE_POP);
		fxScopeCodingBlock(self->statementScope, param);
		fxScopeCodeDefineNodes(self->statementScope, param);
		fxScopeCodeUsingStatement(self->statementScope, param, self->statement);
		fxScopeCoded(self->statementScope, param);
		fxScopeCoded(self->scope, param);
	}
	else {
		fxScopeCodingBlock(self->statementScope, param);
		fxScopeCodeDefineNodes(self->statementScope, param);
		fxScopeCodeUsingStatement(self->statementScope, param, self->statement);
		fxScopeCoded(self->statementScope, param);
	}
}

void fxChainNodeCode(void* it, void* param)
{
	txUnaryExpressionNode* self = it;
	txCoder* coder = param;
	txTargetCode* chainTarget = coder->chainTarget;
	coder->chainTarget = fxCoderCreateTarget(param);
	fxNodeDispatchCode(self->right, param);
	fxCoderAdd(param, 0, coder->chainTarget);
	coder->chainTarget = chainTarget;
}

txFlag fxChainNodeCodeThis(void* it, void* param, txFlag flag)
{
	txUnaryExpressionNode* self = it;
	txCoder* coder = param;
	txTargetCode* chainTarget = coder->chainTarget;
	coder->chainTarget = fxCoderCreateTarget(param);
	flag = fxNodeDispatchCodeThis(self->right, param, flag);
    fxCoderAdd(param, 0, coder->chainTarget);
	coder->chainTarget = chainTarget;
	return flag;
}

void fxClassNodeCode(void* it, void* param) 
{
	txClassNode* self = it;
	txCoder* coder = param;
	txClassNode* former = coder->classNode;
	txFlag flag;
	txInteger prototype = fxCoderUseTemporaryVariable(coder);
	txInteger constructor = fxCoderUseTemporaryVariable(coder);
	txDeclareNode* declaration = self->scope->firstDeclareNode;
	txNode* item = self->items->first;
	if (self->symbol)
		fxScopeCodingBlock(self->symbolScope, param);
	if (self->heritage) {
		if (self->heritage->description->token == XS_TOKEN_HOST) {
			fxCoderAddByte(param, 1, XS_CODE_NULL);
			fxNodeDispatchCode(self->heritage, param);
		}
		else {
			fxNodeDispatchCode(self->heritage, param);
			fxCoderAddByte(param, 1, XS_CODE_EXTEND);
		}
	}
	else {
		fxCoderAddByte(param, 1, XS_CODE_NULL);
		fxCoderAddByte(param, 1, XS_CODE_OBJECT);
	}
	fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, prototype);
	fxScopeCodingBlock(self->scope, param);
	
	coder->classNode = self;
	fxNodeDispatchCode(self->constructor, param);
	
	fxCoderAddByte(param, 0, XS_CODE_TO_INSTANCE);
	fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, constructor);
	fxCoderAddByte(param, -3, XS_CODE_CLASS);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, constructor);
	if (self->symbol)
		fxCoderAddSymbol(param, 0, XS_CODE_NAME, self->symbol);
		
	while (item) {
		if (item->description->token == XS_TOKEN_PROPERTY) {
			txPropertyNode* property = (txPropertyNode*)item;
			if (item->flags & (mxMethodFlag | mxGetterFlag | mxSetterFlag)) {
				if (item->flags & mxStaticFlag)
					fxCoderAddByte(param, 1, XS_CODE_DUB);
				else
					fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, prototype);
				fxNodeDispatchCode(property->value, param);
				fxCoderAddSymbol(param, -2, XS_CODE_NEW_PROPERTY, property->symbol);
				flag = XS_DONT_ENUM_FLAG;
				if (item->flags & mxMethodFlag)
					flag |= XS_NAME_FLAG | XS_METHOD_FLAG;
				else if (item->flags & mxGetterFlag)
					flag |= XS_NAME_FLAG | XS_METHOD_FLAG | XS_GETTER_FLAG;
				else if (item->flags & mxSetterFlag)
					flag |= XS_NAME_FLAG | XS_METHOD_FLAG | XS_SETTER_FLAG;
				fxCoderAddInteger(param, 0, XS_CODE_INTEGER_1, flag);
			}
		}
		else if (item->description->token == XS_TOKEN_PROPERTY_AT) {
			txPropertyAtNode* property = (txPropertyAtNode*)item;
			if (item->flags & (mxMethodFlag | mxGetterFlag | mxSetterFlag)) {
				if (item->flags & mxStaticFlag)
					fxCoderAddByte(param, 1, XS_CODE_DUB);
				else
					fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, prototype);
				fxNodeDispatchCode(property->at, param);
				fxCoderAddByte(param, 0, XS_CODE_AT);
				fxNodeDispatchCode(property->value, param);
				fxCoderAddByte(param, -3, XS_CODE_NEW_PROPERTY_AT);
				flag = XS_DONT_ENUM_FLAG;
				if (item->flags & mxMethodFlag)
					flag |= XS_NAME_FLAG | XS_METHOD_FLAG;
				else if (item->flags & mxGetterFlag)
					flag |= XS_NAME_FLAG | XS_METHOD_FLAG | XS_GETTER_FLAG;
				else if (item->flags & mxSetterFlag)
					flag |= XS_NAME_FLAG | XS_METHOD_FLAG | XS_SETTER_FLAG;
				fxCoderAddInteger(param, 0, XS_CODE_INTEGER_1, flag);
			}
			else {
				fxNodeDispatchCode(property->at, param);
				fxCoderAddByte(param, 0, XS_CODE_AT);
				fxCoderAddIndex(param, 0, XS_CODE_CONST_CLOSURE_1, declaration->index);
				fxCoderAddByte(param, -1, XS_CODE_POP);
				declaration = declaration->nextDeclareNode;
			}
		}
		else  {
			txPrivatePropertyNode* property = (txPrivatePropertyNode*)item;
			fxCoderAddIndex(param, 0, XS_CODE_CONST_CLOSURE_1, declaration->index);
			declaration = declaration->nextDeclareNode;
			if (item->flags & (mxMethodFlag | mxGetterFlag | mxSetterFlag)) {
				fxNodeDispatchCode(property->value, param);
				fxCoderAddIndex(param, 0, XS_CODE_CONST_CLOSURE_1, declaration->index);
				fxCoderAddByte(param, -1, XS_CODE_POP);
				declaration = declaration->nextDeclareNode;
			}
		}
		item = item->next;
	}
	if (self->symbol)
		fxCoderAddIndex(param, 0, XS_CODE_CONST_CLOSURE_1, self->symbolScope->firstDeclareNode->index);
	if (self->instanceInit) {
		fxNodeDispatchCode(self->instanceInit, param);
		fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, prototype);
		fxCoderAddByte(param, -1, XS_CODE_SET_HOME);
		fxCoderAddIndex(param, 0, XS_CODE_CONST_CLOSURE_1, declaration->index);
		fxCoderAddByte(param, -1, XS_CODE_POP);
	}
	if (self->constructorInit) {
		fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, constructor);
		fxNodeDispatchCode(self->constructorInit, param);
		fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, constructor);
		fxCoderAddByte(param, -1, XS_CODE_SET_HOME);
		fxCoderAddByte(param, 1, XS_CODE_CALL);
		fxCoderAddInteger(param, -2, XS_CODE_RUN_1, 0);
		fxCoderAddByte(param, -1, XS_CODE_POP);
	}
	coder->classNode = former;
	fxScopeCoded(self->scope, param);
	if (self->symbol)
		fxScopeCoded(self->symbolScope, param);
	fxCoderUnuseTemporaryVariables(coder, 2);
}

void fxCoalesceExpressionNodeCode(void* it, void* param) 
{
	txBinaryExpressionNode* self = it;
	txTargetCode* endTarget = fxCoderCreateTarget(param);
	self->right->flags |= (self->flags & mxTailRecursionFlag);
	fxNodeDispatchCode(self->left, param);
	fxCoderAddBranch(param, -1, XS_CODE_BRANCH_COALESCE_1, endTarget);
	fxNodeDispatchCode(self->right, param);
	fxCoderAdd(param, 0, endTarget);
}

void fxCompoundExpressionNodeCode(void* it, void* param) 
{
	txAssignNode* self = it;
	txCoder* coder = param;
	txToken token = self->description->token;
	txFlag shortcut = ((token == XS_TOKEN_AND_ASSIGN) || (token == XS_TOKEN_COALESCE_ASSIGN) || (token == XS_TOKEN_OR_ASSIGN)) ? 1 : 0;
	txTargetCode* elseTarget = (shortcut) ? fxCoderCreateTarget(param) : C_NULL;
	txTargetCode* endTarget = (shortcut) ? fxCoderCreateTarget(param) : C_NULL;
	txInteger stackLevel = 0;
	txFlag swap = fxNodeDispatchCodeThis(self->reference, param, 1);
	switch (self->description->token) {
	case XS_TOKEN_AND_ASSIGN:
		fxCoderAddByte(param, 1, XS_CODE_DUB);
		fxCoderAddBranch(param, -1, XS_CODE_BRANCH_ELSE_1, elseTarget);
		fxCoderAddByte(param, -1, XS_CODE_POP);
		fxNodeDispatchCode(self->value, param);
		fxCompoundExpressionNodeCodeName(it, param);
		stackLevel = coder->stackLevel;
		break;
	case XS_TOKEN_COALESCE_ASSIGN:
		fxCoderAddBranch(param, -1, XS_CODE_BRANCH_COALESCE_1, elseTarget);
		fxNodeDispatchCode(self->value, param);
		fxCompoundExpressionNodeCodeName(it, param);
		stackLevel = coder->stackLevel;
		break;
	case XS_TOKEN_OR_ASSIGN:
		fxCoderAddByte(param, 1, XS_CODE_DUB);
		fxCoderAddBranch(param, -1, XS_CODE_BRANCH_IF_1, elseTarget);
		fxCoderAddByte(param, -1, XS_CODE_POP);
		fxNodeDispatchCode(self->value, param);
		fxCompoundExpressionNodeCodeName(it, param);
		stackLevel = coder->stackLevel;
		break;
	default:
		fxNodeDispatchCode(self->value, param);
		fxCoderAddByte(param, -1, self->description->code);
		break;
	}
	fxNodeDispatchCodeAssign(self->reference, param, 0);
	if (shortcut) {
		fxCoderAddBranch(param, 0, XS_CODE_BRANCH_1, endTarget);
		coder->stackLevel = stackLevel;
		fxCoderAdd(param, 0, elseTarget);
		while (swap > 0) {
			if (!(self->flags & mxExpressionNoValue))
				fxCoderAddByte(param, 0, XS_CODE_SWAP);
			fxCoderAddByte(param, -1, XS_CODE_POP);
			swap--;
		}
		fxCoderAdd(param, 0, endTarget);
	}
}

void fxCompoundExpressionNodeCodeName(void* it, void* param) 
{
	txAssignNode* self = it;
	txAccessNode* reference = (txAccessNode*)(self->reference);
	txToken token = reference->description->token;
	txNode* value = self->value;
	if (token != XS_TOKEN_ACCESS)
		return;
	if (fxNodeCodeName(value))
		fxCoderAddSymbol(param, 0, XS_CODE_NAME, reference->symbol);
}

void fxDebuggerNodeCode(void* it, void* param) 
{
	fxCoderAddByte(param, 0, XS_CODE_DEBUGGER);
}

void fxDeclareNodeCode(void* it, void* param) 
{
	txDeclareNode* self = it;
	txCoder* coder = param;
	if (self->description->token == XS_TOKEN_CONST)
		fxReportParserError(coder->parser, self->line, "invalid const");
	else if (self->description->token == XS_TOKEN_LET) {
		fxNodeDispatchCodeReference(self, param, 0);
		fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
		fxNodeDispatchCodeAssign(self, param, 0);
		fxCoderAddByte(coder, -1, XS_CODE_POP);
	}
	else if (self->description->token == XS_TOKEN_USING)
		fxReportParserError(coder->parser, self->line, "invalid using");
}

void fxDeclareNodeCodeAssign(void* it, void* param, txFlag flag) 
{
	txDeclareNode* self = it;
	txDeclareNode* declaration = self->declaration;
	if (!declaration)
		fxCoderAddSymbol(param, -1, XS_CODE_SET_VARIABLE, self->symbol);
	else {
		if (self->description->token == XS_TOKEN_CONST)
			fxCoderAddIndex(param, 0, (declaration->flags & mxDeclareNodeClosureFlag) ? XS_CODE_CONST_CLOSURE_1: XS_CODE_CONST_LOCAL_1, declaration->index);
		else if (self->description->token == XS_TOKEN_LET)
			fxCoderAddIndex(param, 0, (declaration->flags & mxDeclareNodeClosureFlag) ? XS_CODE_LET_CLOSURE_1: XS_CODE_LET_LOCAL_1, declaration->index);
		else if (self->description->token == XS_TOKEN_USING) {
			fxCoderAddIndex(param, 0, (declaration->flags & mxDeclareNodeClosureFlag) ? XS_CODE_CONST_CLOSURE_1: XS_CODE_CONST_LOCAL_1, declaration->index);
			if (self->flags & mxAwaitingFlag)
				fxCoderAddByte(param, 0, XS_CODE_USING_ASYNC);
			else
				fxCoderAddByte(param, 0, XS_CODE_USING);
			fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, declaration->nextDeclareNode->index);
		}
		else
			fxCoderAddIndex(param, 0, (declaration->flags & mxDeclareNodeClosureFlag) ? XS_CODE_VAR_CLOSURE_1 : XS_CODE_VAR_LOCAL_1, declaration->index);
	}
}

void fxDeclareNodeCodeReference(void* it, void* param, txFlag flag) 
{
	txAccessNode* self = it;
	txCoder* coder = param;
	txDeclareNode* declaration = self->declaration;
	if (!declaration) {
		if (coder->evalFlag)
			fxCoderAddSymbol(param, 1, XS_CODE_EVAL_REFERENCE, self->symbol);
		else
			fxCoderAddSymbol(param, 1, XS_CODE_PROGRAM_REFERENCE, self->symbol);
	}
}

void fxDefineNodeCode(void* it, void* param) 
{
	txDefineNode* self = it;
	txCoder* coder = param;
	if (self->flags & mxDefineNodeCodedFlag)
		return;
	self->flags |= mxDefineNodeCodedFlag;
	fxDeclareNodeCodeReference(it, param, 0);
	fxNodeDispatchCode(self->initializer, coder);
    self->initializer = C_NULL;
	fxDeclareNodeCodeAssign(it, param, 0);
	fxCoderAddByte(coder, -1, XS_CODE_POP);
}

void fxDelegateNodeCode(void* it, void* param)
{
	txStatementNode* self = it;
	txBoolean async = (self->flags & mxAsyncFlag) ? 1 : 0;
	txCoder* coder = param;
	txInteger iterator;
	txInteger method;
	txInteger next;
	txInteger result;

	txTargetCode* nextTarget = fxCoderCreateTarget(param);
	txTargetCode* catchTarget = fxCoderCreateTarget(param);
	txTargetCode* rethrowTarget = fxCoderCreateTarget(param);
	txTargetCode* returnTarget = fxCoderCreateTarget(param);
	txTargetCode* normalTarget = fxCoderCreateTarget(param);
	txTargetCode* doneTarget = fxCoderCreateTarget(param);
	
	iterator = fxCoderUseTemporaryVariable(param);
	method = fxCoderUseTemporaryVariable(param);
	next = fxCoderUseTemporaryVariable(param);
	result = fxCoderUseTemporaryVariable(param);
	
	fxNodeDispatchCode(self->expression, param);
	if (async)
		fxCoderAddByte(param, 0, XS_CODE_FOR_AWAIT_OF);
	else
		fxCoderAddByte(param, 0, XS_CODE_FOR_OF);
	fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, iterator);
	fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->nextSymbol);
	fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, next);
	fxCoderAddByte(param, -1, XS_CODE_POP);
	
	fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
	fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, result);
	fxCoderAddByte(param, -1, XS_CODE_POP);
	fxCoderAddBranch(param, 0, XS_CODE_CATCH_1, catchTarget);
	fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, normalTarget);
	
// LOOP	
	fxCoderAdd(param, 0, nextTarget);
	if (async)
		fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->valueSymbol);
	fxCoderAddByte(coder, 0, XS_CODE_YIELD_STAR);
	fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, result);
	fxCoderAddByte(param, -1, XS_CODE_POP);
	fxCoderAddBranch(param, 0, XS_CODE_CATCH_1, catchTarget);
	fxCoderAddBranch(coder, 1, XS_CODE_BRANCH_STATUS_1, normalTarget);
	
// RETURN	
	fxCoderAddByte(param, 0, XS_CODE_UNCATCH);
	if (async) {
		fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, result);
		fxCoderAddByte(param, 0, XS_CODE_AWAIT);
		fxCoderAddByte(param, 0, XS_CODE_THROW_STATUS);
		fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, result);
		fxCoderAddByte(param, -1, XS_CODE_POP);
	}	
	
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, iterator);
	fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->returnSymbol);
	fxCoderAddBranch(param, 0, XS_CODE_BRANCH_CHAIN_1, returnTarget);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, iterator);
	fxCoderAddByte(param, 0, XS_CODE_SWAP);
	fxCoderAddByte(param, 1, XS_CODE_CALL);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, result);
	fxCoderAddInteger(param, -3, XS_CODE_RUN_1, 1);
	if (async) {
		fxCoderAddByte(param, 0, XS_CODE_AWAIT);
		fxCoderAddByte(param, 0, XS_CODE_THROW_STATUS);
	}
	fxCoderAddByte(param, 0, XS_CODE_CHECK_INSTANCE);
	fxCoderAddByte(param, 1, XS_CODE_DUB);
	fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->doneSymbol);
	fxCoderAddBranch(param, -1, XS_CODE_BRANCH_ELSE_1, nextTarget);
	fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->valueSymbol);
	fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, result);
	fxCoderAdd(coder, 0, returnTarget);
	fxCoderAddByte(param, -1, XS_CODE_POP);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, result);
	if (async) {
		fxCoderAddByte(param, 0, XS_CODE_AWAIT);
		fxCoderAddByte(param, 0, XS_CODE_THROW_STATUS);
	}	
	fxCoderAddByte(param, -1, XS_CODE_SET_RESULT);
	fxCoderAdjustEnvironment(coder, coder->returnTarget);
	fxCoderAdjustScope(coder, coder->returnTarget);
	fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, coder->returnTarget);
	
// THROW	
	fxCoderAdd(coder, 0, catchTarget);

	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, iterator);
	fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->throwSymbol);
	fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, method);
	fxCoderAddBranch(param, -1, XS_CODE_BRANCH_COALESCE_1, doneTarget);
	
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, iterator);
	fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->returnSymbol);
	fxCoderAddBranch(param, -1, XS_CODE_BRANCH_CHAIN_1, rethrowTarget);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, iterator);
	fxCoderAddByte(param, 0, XS_CODE_SWAP);
	fxCoderAddByte(param, 1, XS_CODE_CALL);
	fxCoderAddInteger(param, -2, XS_CODE_RUN_1, 0);
	if (async) {
		fxCoderAddByte(param, 0, XS_CODE_AWAIT);
		fxCoderAddByte(param, 0, XS_CODE_THROW_STATUS);
	}
	fxCoderAddByte(param, 0, XS_CODE_CHECK_INSTANCE);
	fxCoderAdd(coder, 0, rethrowTarget);
	fxCoderAddByte(param, -1, XS_CODE_POP);
	
	fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
	fxCoderAddByte(param, 0, XS_CODE_CHECK_INSTANCE);
	fxCoderAddByte(param, -1, XS_CODE_POP);
	
// NORMAL	
	fxCoderAdd(coder, 0, normalTarget);
	fxCoderAddByte(param, 0, XS_CODE_UNCATCH);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, next);
	fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, method);
	fxCoderAdd(param, 1, doneTarget);
	fxCoderAddByte(param, -1, XS_CODE_POP);

	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, iterator);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, method);
	fxCoderAddByte(param, 1, XS_CODE_CALL);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, result);
	fxCoderAddInteger(param, -3, XS_CODE_RUN_1, 1);
	if (async) {
		fxCoderAddByte(param, 0, XS_CODE_AWAIT);
		fxCoderAddByte(param, 0, XS_CODE_THROW_STATUS);
	}
	fxCoderAddByte(param, 0, XS_CODE_CHECK_INSTANCE);
	fxCoderAddByte(param, 1, XS_CODE_DUB);
	fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->doneSymbol);
	fxCoderAddBranch(param, -1, XS_CODE_BRANCH_ELSE_1, nextTarget);
	fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->valueSymbol);

	fxCoderUnuseTemporaryVariables(param, 4);
}

void fxDeleteNodeCode(void* it, void* param) 
{
	txDeleteNode* self = it;
	fxNodeDispatchCodeDelete(self->reference, param);
}

void fxDoNodeCode(void* it, void* param) 
{
	txDoNode* self = it;
	txCoder* coder = param;
	txTargetCode* loopTarget = fxCoderCreateTarget(param);
	if (coder->programFlag) {
		fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
		fxCoderAddByte(param, -1, XS_CODE_SET_RESULT);
	}
	fxCoderAdd(param, 0, loopTarget);
	fxNodeDispatchCode(self->statement, param);
	fxCoderAdd(param, 0, coder->firstContinueTarget);
	fxNodeDispatchCode(self->expression, param);
	fxCoderAddBranch(param, -1, XS_CODE_BRANCH_IF_1, loopTarget);
}

void fxExportNodeCode(void* it, void* param) 
{
}

void fxExpressionsNodeCode(void* it, void* param) 
{
	txExpressionsNode* self = it;
	if (self->items) {
		txNode* item = self->items->first;
		txNode* previous = NULL;
		txNode* next;
		while (item) {
			next = item->next;
			if (previous)
				fxCoderAddByte(param, -1, XS_CODE_POP);
			if (!next)
				item->flags |= (self->flags & mxTailRecursionFlag);
			fxNodeDispatchCode(item, param);
			previous = item;
			item = next;
		}
	}
}

txFlag fxExpressionsNodeCodeThis(void* it, void* param, txFlag flag) 
{
	txExpressionsNode* self = it;
 	if (self->items) {
 		txNode* item = self->items->first;
 		if (item->next == C_NULL) {
 			return fxNodeDispatchCodeThis(item, param, flag);
 		}
 	}
	return fxNodeCodeThis(it, param, flag);
}

void fxExpressionsNodeCodeDelete(void* it, void* param) 
 {
 	txExpressionsNode* self = it;
 	if (self->items) {
 		txNode* item = self->items->first;
 		if (item->next == C_NULL) {
			fxNodeDispatchCodeDelete(item, param);
 			return;
 		}
 	}
	fxNodeCodeDelete(it, param);
 }

void fxFieldNodeCode(void* it, void* param) 
{
	txFieldNode* self = it;
	txNode* item = self->item;
	fxCoderAddByte(param, 1, XS_CODE_THIS);
	if (item->description->token == XS_TOKEN_PROPERTY) {
		fxNodeDispatchCode(self->value, param);
		fxCoderAddSymbol(param, -2, XS_CODE_NEW_PROPERTY, ((txPropertyNode*)item)->symbol);
		fxCoderAddInteger(param, 0, XS_CODE_INTEGER_1, fxNodeCodeName(self->value) ? XS_NAME_FLAG : 0);
	}
	else if (item->description->token == XS_TOKEN_PROPERTY_AT) {
		fxCoderAddIndex(param, 1, XS_CODE_GET_CLOSURE_1, ((txPropertyAtNode*)item)->atAccess->declaration->index);
		fxNodeDispatchCode(self->value, param);
		fxCoderAddByte(param, -3, XS_CODE_NEW_PROPERTY_AT);
		fxCoderAddInteger(param, 0, XS_CODE_INTEGER_1, fxNodeCodeName(self->value) ? XS_NAME_FLAG : 0);
	}
	else {
		if (item->flags & (mxMethodFlag | mxGetterFlag | mxSetterFlag))
			fxCoderAddIndex(param, 1, XS_CODE_GET_CLOSURE_1, ((txPrivatePropertyNode*)item)->valueAccess->declaration->index);
		else
			fxNodeDispatchCode(self->value, param);
		fxCoderAddIndex(param, -2, XS_CODE_NEW_PRIVATE_1, ((txPrivatePropertyNode*)item)->symbolAccess->declaration->index);
		if (item->flags & mxMethodFlag)
			fxCoderAddInteger(param, 0, XS_CODE_INTEGER_1, XS_NAME_FLAG | XS_METHOD_FLAG);
		else if (item->flags & mxGetterFlag)
			fxCoderAddInteger(param, 0, XS_CODE_INTEGER_1, XS_NAME_FLAG | XS_METHOD_FLAG | XS_GETTER_FLAG);
		else if (item->flags & mxSetterFlag)
			fxCoderAddInteger(param, 0, XS_CODE_INTEGER_1, XS_NAME_FLAG | XS_METHOD_FLAG | XS_SETTER_FLAG);
		else
			fxCoderAddInteger(param, 0, XS_CODE_INTEGER_1, fxNodeCodeName(self->value) ? XS_NAME_FLAG : 0);
	}
}

void fxForNodeCode(void* it, void* param) 
{
	txForNode* self = it;
	txCoder* coder = param;
	txTargetCode* continueTarget;
	txTargetCode* nextTarget;
	txTargetCode* doneTarget;
	txUsingContext context;
	
	continueTarget = coder->firstContinueTarget;
	coder->firstContinueTarget = continueTarget->nextTarget;
	continueTarget->nextTarget = C_NULL;
	
	fxScopeCodingBlock(self->scope, param);
	fxScopeCodeDefineNodes(self->scope, param);
	if (self->scope->disposableNodeCount)
		fxScopeCodeUsing(self->scope, coder, &context);	
	nextTarget = fxCoderCreateTarget(param);
	doneTarget = fxCoderCreateTarget(param);
	if (self->initialization)
		fxNodeDispatchCode(self->initialization, param);
	if (coder->programFlag) {
		fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
		fxCoderAddByte(param, -1, XS_CODE_SET_RESULT);
	}
	fxScopeCodeRefresh(self->scope, param);
	fxCoderAdd(param, 0, nextTarget);
	if (self->expression) {
		fxNodeDispatchCode(self->expression, param);
		fxCoderAddBranch(param, -1, XS_CODE_BRANCH_ELSE_1, doneTarget);
	}
	
	continueTarget->environmentLevel = coder->environmentLevel;
	continueTarget->scopeLevel = coder->scopeLevel;
	continueTarget->stackLevel = coder->stackLevel;
	continueTarget->nextTarget = coder->firstContinueTarget;
	coder->firstContinueTarget = continueTarget;
	fxNodeDispatchCode(self->statement, param);
	fxCoderAdd(param, 0, continueTarget);
	coder->firstContinueTarget = continueTarget->nextTarget;
	continueTarget->nextTarget = C_NULL;
	
	if (self->iteration) {
		fxScopeCodeRefresh(self->scope, param);
		self->iteration->flags |= mxExpressionNoValue;
		fxNodeDispatchCode(self->iteration, param);
		fxCoderAddByte(param, -1, XS_CODE_POP);
	}
	fxCoderAddBranch(param, 0, XS_CODE_BRANCH_1, nextTarget);
	fxCoderAdd(param, 0, doneTarget);
	if (self->scope->disposableNodeCount)
		fxScopeCodeUsed(self->scope, coder, &context);
	fxScopeCoded(self->scope, param);
	
	continueTarget->nextTarget = coder->firstContinueTarget;
	coder->firstContinueTarget = continueTarget;
}

void fxForInForOfNodeCode(void* it, void* param) 
{
	txForInForOfNode* self = it;
	txCoder* coder = param;
	txBoolean async = (self->description->code == XS_CODE_FOR_AWAIT_OF) ? 1 : 0;
	txTargetCode* continueTarget;
	txInteger iterator;
	txInteger next;
	txInteger done;
	txInteger result;
	txInteger exception;
	txInteger selector;
	txInteger selection;
	txTargetCode* nextTarget;
	txTargetCode* returnTarget;
	txTargetCode* doneTarget;
	txTargetCode* catchTarget;
	txTargetCode* normalTarget;
	txTargetCode* uncatchTarget;
	txTargetCode* finallyTarget;
	txTargetCode* elseTarget;
	
	iterator = fxCoderUseTemporaryVariable(param);
	next = fxCoderUseTemporaryVariable(param);
	done = fxCoderUseTemporaryVariable(param);
	result = fxCoderUseTemporaryVariable(param);
	exception = fxCoderUseTemporaryVariable(coder);
	selector = fxCoderUseTemporaryVariable(coder);
	
	continueTarget = coder->firstContinueTarget;
	coder->firstContinueTarget = continueTarget->nextTarget;
	continueTarget->nextTarget = C_NULL;
	
	fxScopeCodingBlock(self->scope, param);
	fxScopeCodeDefineNodes(self->scope, param);

	if (coder->programFlag) {
		fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
		fxCoderAddByte(param, -1, XS_CODE_SET_RESULT);
	}
	fxNodeDispatchCode(self->expression, param);
	fxCoderAddByte(param, 0, self->description->code);
	fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, iterator);
	fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->nextSymbol);
	fxCoderAddIndex(param, 0, XS_CODE_PULL_LOCAL_1, next);

	coder->firstBreakTarget = fxCoderAliasTargets(param, coder->firstBreakTarget);
	coder->firstContinueTarget = fxCoderAliasTargets(param, coder->firstContinueTarget);
	coder->returnTarget = fxCoderAliasTargets(param, coder->returnTarget);
	catchTarget = fxCoderCreateTarget(param);
	normalTarget = fxCoderCreateTarget(param);
	fxCoderAddBranch(param, 0, XS_CODE_CATCH_1, catchTarget);
	
// LOOP	
	nextTarget = fxCoderCreateTarget(param);
	fxCoderAdd(param, 0, nextTarget);
	fxCoderAddByte(param, 1, XS_CODE_TRUE);
	fxCoderAddIndex(param, -1, XS_CODE_PULL_LOCAL_1, done);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, iterator);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, next);
	fxCoderAddByte(param, 1, XS_CODE_CALL);
	fxCoderAddInteger(param, -2, XS_CODE_RUN_1, 0);
	if (async) {
		fxCoderAddByte(param, 0, XS_CODE_AWAIT);
		fxCoderAddByte(param, 0, XS_CODE_THROW_STATUS);
	}
	fxCoderAddByte(param, 0, XS_CODE_CHECK_INSTANCE);
	fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, result);
	fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->doneSymbol);
	fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, done);
	fxCoderAddBranch(param, -1, XS_CODE_BRANCH_IF_1, normalTarget);

	fxScopeCodeReset(self->scope, param);
	fxNodeDispatchCodeReference(self->reference, param, 0);
	fxCoderAddByte(param, 1, XS_CODE_TRUE);
	fxCoderAddIndex(param, -1, XS_CODE_PULL_LOCAL_1, done);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, result);
	fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->valueSymbol);
	fxCoderAddByte(param, 1, XS_CODE_FALSE);
	fxCoderAddIndex(param, -1, XS_CODE_PULL_LOCAL_1, done);
	fxNodeDispatchCodeAssign(self->reference, param, 0);
	fxCoderAddByte(param, -1, XS_CODE_POP);

	continueTarget->environmentLevel = coder->environmentLevel;
	continueTarget->scopeLevel = coder->scopeLevel;
	continueTarget->stackLevel = coder->stackLevel;
	continueTarget->nextTarget = coder->firstContinueTarget;
	coder->firstContinueTarget = continueTarget;
	fxNodeDispatchCode(self->statement, param);
	fxCoderAdd(param, 0, coder->firstContinueTarget);
	coder->firstContinueTarget = continueTarget->nextTarget;
	continueTarget->nextTarget = C_NULL;
	
	fxScopeCodeUsedReverse(self->scope, coder, self->scope->firstDeclareNode, exception, selector);

	fxCoderAddBranch(param, 0, XS_CODE_BRANCH_1, nextTarget);

//	 PRE FINALLY
	uncatchTarget = fxCoderCreateTarget(param);
	finallyTarget = fxCoderCreateTarget(param);

	fxCoderAdd(coder, 0, catchTarget);
	fxCoderAddByte(coder, 1, XS_CODE_EXCEPTION);
	fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, exception);
	fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, 0);
	fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, selector);
	fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, finallyTarget);
	selection = 1;
	coder->firstBreakTarget = fxCoderFinalizeTargets(param, coder->firstBreakTarget, selector, &selection, uncatchTarget);
	coder->firstContinueTarget = fxCoderFinalizeTargets(param, coder->firstContinueTarget, selector, &selection, uncatchTarget);
	coder->returnTarget = fxCoderFinalizeTargets(param, coder->returnTarget, selector, &selection, uncatchTarget);
	fxCoderAdd(param, 0, normalTarget);
	fxCoderAddInteger(param, 1, XS_CODE_INTEGER_1, selection);
	fxCoderAddIndex(param, -1, XS_CODE_PULL_LOCAL_1, selector);
	fxCoderAdd(coder, 0, uncatchTarget);
	fxCoderAddByte(param, 0, XS_CODE_UNCATCH);
	fxCoderAdd(param, 0, finallyTarget);
	
//	 FINALLY
	catchTarget = fxCoderCreateTarget(param);
	normalTarget = fxCoderCreateTarget(param);

	fxCoderAddBranch(param, 0, XS_CODE_CATCH_1, catchTarget);

	doneTarget = fxCoderCreateTarget(param);
	returnTarget = fxCoderCreateTarget(param);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, done);
	fxCoderAddBranch(param, -1, XS_CODE_BRANCH_IF_1, doneTarget);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, iterator);
	fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->returnSymbol);
	fxCoderAddBranch(param, 0, XS_CODE_BRANCH_CHAIN_1, returnTarget);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, iterator);
	fxCoderAddByte(param, 0, XS_CODE_SWAP);
	fxCoderAddByte(param, 1, XS_CODE_CALL);
	fxCoderAddInteger(param, -2, XS_CODE_RUN_1, 0);
	if (async) {
		fxCoderAddByte(param, 0, XS_CODE_AWAIT);
		fxCoderAddByte(param, 0, XS_CODE_THROW_STATUS);
	}
 	fxCoderAddByte(param, 0, XS_CODE_CHECK_INSTANCE);
	fxCoderAdd(param, 0, returnTarget);
	fxCoderAddByte(param, -1, XS_CODE_POP);
	fxCoderAdd(param, 0, doneTarget);
	
	fxCoderAddByte(coder, 0, XS_CODE_UNCATCH);
	fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, normalTarget);
	
	fxCoderAdd(coder, 0, catchTarget);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, selector);
	fxCoderAddBranch(param, -1, XS_CODE_BRANCH_ELSE_1, normalTarget);
	fxCoderAddByte(coder, 1, XS_CODE_EXCEPTION);
	fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, exception);
	fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, 0);
	fxCoderAddIndex(coder, -1, XS_CODE_PULL_LOCAL_1, selector);
	fxCoderAdd(param, 0, normalTarget);

	fxScopeCodeUsedReverse(self->scope, coder, self->scope->firstDeclareNode, exception, selector);

//	 POST FINALLY
	elseTarget = fxCoderCreateTarget(param);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, selector);
	fxCoderAddBranch(param, -1, XS_CODE_BRANCH_IF_1, elseTarget);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, exception);
	fxCoderAddByte(param, -1, XS_CODE_THROW);
	fxCoderAdd(param, 0, elseTarget);
	selection = 1;
	fxCoderJumpTargets(param, coder->firstBreakTarget, selector, &selection);
	fxCoderJumpTargets(param, coder->firstContinueTarget, selector, &selection);
	fxCoderJumpTargets(param, coder->returnTarget, selector, &selection);
	
	fxScopeCoded(self->scope, param);
	continueTarget->nextTarget = coder->firstContinueTarget;
	coder->firstContinueTarget = continueTarget;
	
	fxCoderUnuseTemporaryVariables(param, 6);
}

void fxFunctionNodeCode(void* it, void* param) 
{
	txFunctionNode* self = it;
	txCoder* coder = param;
	txInteger environmentLevel = coder->environmentLevel;
	txBoolean evalFlag = coder->evalFlag;
	txInteger line = coder->line;
	txBoolean programFlag = coder->programFlag;
	txInteger scopeLevel = coder->scopeLevel;
	txTargetCode* firstBreakTarget = coder->firstBreakTarget;
	txTargetCode* firstContinueTarget = coder->firstContinueTarget;
	txTargetCode* returnTarget = coder->returnTarget;
	txSymbol* name = self->symbol;
	txTargetCode* target = fxCoderCreateTarget(param);
	
	if ((self->flags & mxEvalFlag) && !(self->flags & mxStrictFlag))
		coder->evalFlag = 1;
	coder->line = kNoLine;
	coder->programFlag = 0;
	coder->scopeLevel = 0;
	coder->firstBreakTarget = NULL;
	coder->firstContinueTarget = NULL;

    if (name) {
        if (self->flags & mxGetterFlag) {
            txString buffer = coder->parser->buffer;
            c_strcpy(buffer, "get ");
            c_strcat(buffer, name->string);
            name = fxNewParserSymbol(coder->parser, buffer);
        }
        else if (self->flags & mxSetterFlag) {
            txString buffer = coder->parser->buffer;
            c_strcpy(buffer, "set ");
            c_strcat(buffer, name->string);
            name = fxNewParserSymbol(coder->parser, buffer);
        }
    }
    
	if (self->flags & mxAsyncFlag) {
		if (self->flags & mxGeneratorFlag)
			fxCoderAddSymbol(param, 1, XS_CODE_ASYNC_GENERATOR_FUNCTION, name);
		else
			fxCoderAddSymbol(param, 1, XS_CODE_ASYNC_FUNCTION, name);
	}
	else if (self->flags & mxGeneratorFlag)
		fxCoderAddSymbol(param, 1, XS_CODE_GENERATOR_FUNCTION, name);
	else if (self->flags & (mxArrowFlag | mxMethodFlag | mxGetterFlag | mxSetterFlag))
		fxCoderAddSymbol(param, 1, XS_CODE_FUNCTION, name);
	else
		fxCoderAddSymbol(param, 1, XS_CODE_CONSTRUCTOR_FUNCTION, name);
	if (coder->parser->flags & mxDebugFlag)
		fxCoderAddByte(param, 0, XS_CODE_PROFILE);
	fxCoderAddBranch(param, 0, XS_CODE_CODE_1, target);
	if (self->flags & mxFieldFlag)
		fxCoderAddIndex(param, 0, XS_CODE_BEGIN_STRICT_FIELD, fxCoderCountParameters(coder, self->params));
	else if (self->flags & mxDerivedFlag)
		fxCoderAddIndex(param, 0, XS_CODE_BEGIN_STRICT_DERIVED, fxCoderCountParameters(coder, self->params));
	else if (self->flags & mxBaseFlag)
		fxCoderAddIndex(param, 0, XS_CODE_BEGIN_STRICT_BASE, fxCoderCountParameters(coder, self->params));
	else if (self->flags & mxStrictFlag)
		fxCoderAddIndex(param, 0, XS_CODE_BEGIN_STRICT, fxCoderCountParameters(coder, self->params));
	else
		fxCoderAddIndex(param, 0, XS_CODE_BEGIN_SLOPPY, fxCoderCountParameters(coder, self->params));
	coder->path = C_NULL;
	if (self->line != kNoLine)
		fxCoderAddLine(coder, 0, XS_CODE_LINE, it); 
	if (self->scopeCount)
		fxCoderAddIndex(param, 0, XS_CODE_RESERVE_1, self->scopeCount);
	fxScopeCodeRetrieve(self->scope, param);
	fxScopeCodingParams(self->scope, param);
	if ((self->flags & mxAsyncFlag) && !(self->flags & mxGeneratorFlag))
		fxCoderAddByte(param, 0, XS_CODE_START_ASYNC);
	if (self->flags & mxBaseFlag) {
		if (coder->classNode->instanceInitAccess) {
			fxCoderAddByte(param, 1, XS_CODE_THIS);
			fxCoderAddIndex(param, 1, XS_CODE_GET_CLOSURE_1, coder->classNode->instanceInitAccess->declaration->index);
			fxCoderAddByte(param, 1, XS_CODE_CALL);
			fxCoderAddInteger(param, -2, XS_CODE_RUN_1, 0);
			fxCoderAddByte(param, -1, XS_CODE_POP);
		}
	}
	fxNodeDispatchCode(self->params, param);
	if ((coder->parser->flags & mxDebugFlag) && coder->path)
		fxCoderAddIndex(coder, 0, XS_CODE_LINE, 0);
	fxScopeCodeDefineNodes(self->scope, param);
	coder->returnTarget = fxCoderCreateTarget(param);
	if (self->flags & mxGeneratorFlag) {
		if (self->flags & mxAsyncFlag)
			fxCoderAddByte(param, 0, XS_CODE_START_ASYNC_GENERATOR);
		else
			fxCoderAddByte(param, 0, XS_CODE_START_GENERATOR);
	}
	fxNodeDispatchCode(self->body, param);
	fxCoderAdd(param, 0, coder->returnTarget);
	if (self->flags & mxArrowFlag)
		fxCoderAddByte(param, 0, XS_CODE_END_ARROW);
	else if (self->flags & mxBaseFlag)
		fxCoderAddByte(param, 0, XS_CODE_END_BASE);
	else if (self->flags & mxDerivedFlag)
		fxCoderAddByte(param, 0, XS_CODE_END_DERIVED);
	else
		fxCoderAddByte(param, 0, XS_CODE_END);
	fxCoderAdd(param, 0, target);
	
	if ((self->scope->flags & mxEvalFlag) || coder->evalFlag) {
		fxCoderAddByte(coder, 1, XS_CODE_FUNCTION_ENVIRONMENT);
		fxScopeCodeStore(self->scope, param);
		fxCoderAddByte(coder, -1, XS_CODE_POP);
	}
	else if (self->scope->closureNodeCount || ((self->flags & mxArrowFlag) && (self->flags & mxDefaultFlag))) {
		fxCoderAddByte(coder, 1, XS_CODE_ENVIRONMENT);
		fxScopeCodeStore(self->scope, param);
		fxCoderAddByte(coder, -1, XS_CODE_POP);
	}
	if ((self->flags & (mxArrowFlag | mxBaseFlag | mxDerivedFlag | mxGeneratorFlag | mxStrictFlag | mxMethodFlag)) == 0) {
		fxCoderAddByte(param, 1, XS_CODE_DUB);
		fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
		fxCoderAddSymbol(param, -2, XS_CODE_NEW_PROPERTY, coder->parser->callerSymbol);
		fxCoderAddInteger(param, 0, XS_CODE_INTEGER_1, XS_DONT_ENUM_FLAG);
	}
	
	coder->returnTarget = returnTarget;
	coder->firstContinueTarget = firstContinueTarget;
	coder->firstBreakTarget = firstBreakTarget;
	coder->scopeLevel = scopeLevel;
	coder->programFlag = programFlag;
	coder->line = line;
	coder->evalFlag = evalFlag;
	coder->environmentLevel = environmentLevel;
}

void fxHostNodeCode(void* it, void* param) 
{
	txHostNode* self = it;
	txCoder* coder = param;
	txParser* parser = coder->parser;
	if (self->hostIndex < 0) {
		if (self->symbol) {
			if (self->flags & mxGetterFlag) {
				txString buffer = coder->parser->buffer;
				c_strcpy(buffer, "get ");
				c_strcat(buffer, self->symbol->string);
				self->symbol = fxNewParserSymbol(coder->parser, buffer);
			}
			else if (self->flags & mxSetterFlag) {
				txString buffer = coder->parser->buffer;
				c_strcpy(buffer, "set ");
				c_strcat(buffer, self->symbol->string);
				self->symbol = fxNewParserSymbol(coder->parser, buffer);
			}
		}
		if (self->params)
			self->paramsCount = fxCoderCountParameters(coder, self->params);
		else
			self->paramsCount = -1;	
		if (parser->firstHostNode)
			parser->lastHostNode->nextHostNode = self;
		else
			parser->firstHostNode = self;
		parser->lastHostNode = self;
		self->hostIndex = parser->hostNodeIndex;
		parser->hostNodeIndex++;
	}
	fxCoderAddIndex(param, 1, XS_CODE_HOST, self->hostIndex);
}

void fxIfNodeCode(void* it, void* param) 
{
	txIfNode* self = it;
	txCoder* coder = param;
	fxNodeDispatchCode(self->expression, param);
	if (coder->programFlag) {
		txTargetCode* elseTarget = fxCoderCreateTarget(param);
		txTargetCode* endTarget = fxCoderCreateTarget(param);
		fxCoderAddBranch(param, -1, XS_CODE_BRANCH_ELSE_1, elseTarget);
		fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
		fxCoderAddByte(param, -1, XS_CODE_SET_RESULT);
		fxNodeDispatchCode(self->thenStatement, param);
		fxCoderAddBranch(param, 0, XS_CODE_BRANCH_1, endTarget);
		fxCoderAdd(param, 0, elseTarget);
		fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
		fxCoderAddByte(param, -1, XS_CODE_SET_RESULT);
		if (self->elseStatement)
			fxNodeDispatchCode(self->elseStatement, param);
		fxCoderAdd(param, 0, endTarget);
	}
	else {
		if (self->elseStatement) {
			txTargetCode* elseTarget = fxCoderCreateTarget(param);
			txTargetCode* endTarget = fxCoderCreateTarget(param);
			fxCoderAddBranch(param, -1, XS_CODE_BRANCH_ELSE_1, elseTarget);
			fxNodeDispatchCode(self->thenStatement, param);
			fxCoderAddBranch(param, 0, XS_CODE_BRANCH_1, endTarget);
			fxCoderAdd(param, 0, elseTarget);
			fxNodeDispatchCode(self->elseStatement, param);
			fxCoderAdd(param, 0, endTarget);
		}
		else {
			txTargetCode* endTarget = fxCoderCreateTarget(param);
			fxCoderAddBranch(param, -1, XS_CODE_BRANCH_ELSE_1, endTarget);
			fxNodeDispatchCode(self->thenStatement, param);
			fxCoderAdd(param, 0, endTarget);
		}
	}
}

void fxImportNodeCode(void* it, void* param) 
{
}

/* xs_no_recursion (R7/B5c): the root node is dispatched DIRECTLY from
   fxParserCode (bypassing fxNodeDispatchCode), so the converted root
   emitters convert themselves into a pump frame and drain here. The
   stock body below stays as the fallback (still reachable when the
   pump is already running — e.g. a program nested under include/module
   dispatch — or if the gate is ever removed). */
#define XS_NR_ROOT_SHIM(fn, kind) \
	if (!fxNodeWalkRunningCheck(coder->parser) && fxCodeStepKindFor((void*)fn) == kind) { \
		txNodeWalkFrame* base = fxNodeWalkStackTop(coder->parser); \
		int running__ = coder->parser->nodeWalkRunning; \
		coder->parser->nodeWalkRunning = 1; \
		fxNodeWalkPushCode(coder->parser, it, param, kind, 0); \
		fxNodeWalkDrainCode(coder->parser, base); \
		coder->parser->nodeWalkRunning = running__; \
		return; \
	}

void fxImportCallNodeCode(void* it, void* param)
{
	txCoder* coder = param;
	txImportCallNode* self = it;
	fxNodeDispatchCode(self->expression, param);
	if (self->withExpression)
		fxNodeDispatchCode(self->withExpression, param);
	else
		fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
	coder->importFlag = 1;
	fxCoderAddByte(param, -1, XS_CODE_IMPORT);
}

void fxImportMetaNodeCode(void* it, void* param)
{
	txCoder* coder = param;
	coder->importMetaFlag = 1;
	fxCoderAddByte(param, 1, XS_CODE_IMPORT_META);
}

void fxIncludeNodeCode(void* it, void* param) 
{
	txIncludeNode* self = it;
	fxNodeDispatchCode(self->body, param);
}

void fxIntegerNodeCode(void* it, void* param) 
{
	txIntegerNode* self = it;
	fxCoderAddInteger(param, 1, XS_CODE_INTEGER_1, self->value);
}

void fxLabelNodeCode(void* it, void* param) 
{
	txLabelNode* self = it;
	txCoder* coder = param;
	txNode* statement = self->statement;
	txTargetCode* breakTarget;
	while (statement->description->token == XS_TOKEN_LABEL) {
		txLabelNode* former = (txLabelNode*)statement;
		txLabelNode* current = self;
		while (current) {
			if (former->symbol && current->symbol && (former->symbol == current->symbol)) {
				fxReportParserError(coder->parser, current->line, "duplicate label %s", current->symbol->string);
			}
			current = current->nextLabel;
		}
		former->nextLabel = self;
		self = former;
		statement = self->statement;
	}
	breakTarget = coder->firstBreakTarget;
	while (breakTarget) {
		txLabelNode* former = breakTarget->label;
		if (former) {
			txLabelNode* current = self;
			while (current) {
				if (former->symbol && current->symbol && (former->symbol == current->symbol)) {
					fxReportParserError(coder->parser, current->line, "duplicate label %s", current->symbol->string);
				}
				current = current->nextLabel;
			}
		}
		breakTarget = breakTarget->nextTarget;
	}
	breakTarget = fxCoderCreateTarget(coder);
	breakTarget->nextTarget = coder->firstBreakTarget;
	coder->firstBreakTarget = breakTarget;
	breakTarget->label = self;
	if (self->symbol)
		fxNodeDispatchCode(statement, param);
	else {
		txTargetCode* continueTarget = fxCoderCreateTarget(coder);
		continueTarget->nextTarget = coder->firstContinueTarget;
		coder->firstContinueTarget = continueTarget;
		continueTarget->label = self;
		fxNodeDispatchCode(statement, param);
		coder->firstContinueTarget = continueTarget->nextTarget;
	}
	fxCoderAdd(param, 0, coder->firstBreakTarget);
	coder->firstBreakTarget = breakTarget->nextTarget;
}

void fxMemberNodeCode(void* it, void* param) 
{
	txMemberNode* self = it;
	fxNodeDispatchCode(self->reference, param);
	fxCoderAddSymbol(param, 0, (self->reference->flags & mxSuperFlag) ? XS_CODE_GET_SUPER : XS_CODE_GET_PROPERTY, self->symbol);
}

void fxMemberNodeCodeAssign(void* it, void* param, txFlag flag) 
{
	txMemberNode* self = it;
	fxCoderAddSymbol(param, -1, (self->reference->flags & mxSuperFlag) ? XS_CODE_SET_SUPER : XS_CODE_SET_PROPERTY, self->symbol);
}

void fxMemberNodeCodeDelete(void* it, void* param) 
{
	txMemberNode* self = it;
	fxNodeDispatchCode(self->reference, param);
	fxCoderAddSymbol(param, 0, (self->reference->flags & mxSuperFlag) ? XS_CODE_DELETE_SUPER : XS_CODE_DELETE_PROPERTY, self->symbol);
}

void fxMemberNodeCodeReference(void* it, void* param, txFlag flag) 
{
	txMemberNode* self = it;
	fxNodeDispatchCode(self->reference, param);
}

txFlag fxMemberNodeCodeThis(void* it, void* param, txFlag flag) 
{
	txMemberNode* self = it;
	fxNodeDispatchCode(self->reference, param);
	fxCoderAddByte(param, 1, XS_CODE_DUB);
	fxCoderAddSymbol(param, 0, (self->reference->flags & mxSuperFlag) ? XS_CODE_GET_SUPER : XS_CODE_GET_PROPERTY, self->symbol);
	return 1;
}

void fxMemberAtNodeCode(void* it, void* param) 
{
	txMemberAtNode* self = it;
	fxNodeDispatchCode(self->reference, param);
	fxNodeDispatchCode(self->at, param);
	fxCoderAddByte(param, 0, (self->reference->flags & mxSuperFlag) ? XS_CODE_SUPER_AT : XS_CODE_AT);
	fxCoderAddByte(param, -1, (self->reference->flags & mxSuperFlag) ? XS_CODE_GET_SUPER_AT : XS_CODE_GET_PROPERTY_AT);
}

void fxMemberAtNodeCodeAssign(void* it, void* param, txFlag flag) 
{
	txMemberAtNode* self = it;
	if (flag)
		fxCoderAddByte(param, 0, (self->reference->flags & mxSuperFlag) ? XS_CODE_SUPER_AT_2 : XS_CODE_AT_2);
	fxCoderAddByte(param, -2, (self->reference->flags & mxSuperFlag) ? XS_CODE_SET_SUPER_AT : XS_CODE_SET_PROPERTY_AT);
}

void fxMemberAtNodeCodeDelete(void* it, void* param) 
{
	txMemberAtNode* self = it;
	fxMemberAtNodeCodeReference(it, param, (self->reference->flags & mxSuperFlag) ? 1 : 0);
	fxCoderAddByte(param, -1, (self->reference->flags & mxSuperFlag) ? XS_CODE_DELETE_SUPER_AT : XS_CODE_DELETE_PROPERTY_AT);
}

void fxMemberAtNodeCodeReference(void* it, void* param, txFlag flag) 
{
	txMemberAtNode* self = it;
	fxNodeDispatchCode(self->reference, param);
	fxNodeDispatchCode(self->at, param);
	if (!flag)
		fxCoderAddByte(param, 0, (self->reference->flags & mxSuperFlag) ? XS_CODE_SUPER_AT : XS_CODE_AT);
}

txFlag fxMemberAtNodeCodeThis(void* it, void* param, txFlag flag) 
{
	txMemberAtNode* self = it;
	if (flag) {
		fxMemberAtNodeCodeReference(it, param, 0);
		fxCoderAddByte(param, 2, XS_CODE_DUB_AT);
		flag = 2;
	}
	else {
		fxNodeDispatchCode(self->reference, param);
		fxCoderAddByte(param, 1, XS_CODE_DUB);
		fxNodeDispatchCode(self->at, param);
		fxCoderAddByte(param, 0, (self->reference->flags & mxSuperFlag) ? XS_CODE_SUPER_AT : XS_CODE_AT);
	}
	fxCoderAddByte(param, -1, (self->reference->flags & mxSuperFlag) ? XS_CODE_GET_SUPER_AT : XS_CODE_GET_PROPERTY_AT);
	return flag;
}

void fxModuleNodeCode(void* it, void* param) 
{
	txModuleNode* self = it;
	txCoder* coder = param;
	XS_NR_ROOT_SHIM(fxModuleNodeCode, WC_MODULE);
	txTargetCode* target = fxCoderCreateTarget(param);
	txDeclareNode* declaration;
	txInteger count;
	txSymbol* name = /*(coder->parser->flags & mxDebugFlag) ? self->path :*/ C_NULL;
	txFlag flag = 0;
	
	coder->line = kNoLine;
	coder->programFlag = 0;
	coder->scopeLevel = 0;
	coder->firstBreakTarget = NULL;
	coder->firstContinueTarget = NULL;

	count = 0;
	declaration = self->scope->firstDeclareNode;
	while (declaration) {
		if ((declaration->description->token == XS_TOKEN_DEFINE) || (declaration->description->token == XS_TOKEN_VAR))
			count++;
		declaration = declaration->nextDeclareNode;
	}
	if (count) {
		fxCoderAddSymbol(param, 1, XS_CODE_FUNCTION, name);
		if (coder->parser->flags & mxDebugFlag)
			fxCoderAddByte(param, 0, XS_CODE_PROFILE);
		fxCoderAddBranch(param, 0, XS_CODE_CODE_1, target);
		fxCoderAddIndex(param, 0, XS_CODE_BEGIN_STRICT, 0);
		coder->path = C_NULL;
		if (self->line != kNoLine)
			fxCoderAddLine(coder, 0, XS_CODE_LINE, it); 
		if (self->scopeCount)
			fxCoderAddIndex(param, 0, XS_CODE_RESERVE_1, self->scopeCount);
		fxScopeCodeRetrieve(self->scope, param);
		declaration = self->scope->firstDeclareNode;
		while (declaration) {
			if (declaration->description->token == XS_TOKEN_VAR) {
				fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
				fxCoderAddIndex(coder, 0, XS_CODE_VAR_CLOSURE_1, declaration->index);
				fxCoderAddByte(coder, -1, XS_CODE_POP);
			}
			declaration = declaration->nextDeclareNode;
		}
		fxScopeCodeDefineNodes(self->scope, param);
		fxCoderAddByte(param, 0, XS_CODE_END);
		fxCoderAdd(param, 0, target);
		fxCoderAddByte(param, 1, XS_CODE_ENVIRONMENT);
		fxCoderAddByte(param, -1, XS_CODE_POP);
	
		target = fxCoderCreateTarget(param);
		coder->line = kNoLine;
		coder->programFlag = 0;
		coder->scopeLevel = 0;
		coder->firstBreakTarget = NULL;
		coder->firstContinueTarget = NULL;
	}
	else {
		fxCoderAddByte(coder, 1, XS_CODE_NULL);
	}
	
	if (self->flags & mxAwaitingFlag)
		fxCoderAddSymbol(param, 1, XS_CODE_ASYNC_FUNCTION, name);
	else
		fxCoderAddSymbol(param, 1, XS_CODE_FUNCTION, name);
	if (coder->parser->flags & mxDebugFlag)
		fxCoderAddByte(param, 0, XS_CODE_PROFILE);
	fxCoderAddBranch(param, 0, XS_CODE_CODE_1, target);
	fxCoderAddIndex(param, 0, XS_CODE_BEGIN_STRICT, 0);
	coder->path = C_NULL;
	if (self->line != kNoLine)
		fxCoderAddLine(coder, 0, XS_CODE_LINE, it); 
	if (self->scopeCount)
		fxCoderAddIndex(param, 0, XS_CODE_RESERVE_1, self->scopeCount);
	fxScopeCodeRetrieve(self->scope, param);
	
	if (self->flags & mxAwaitingFlag)
		fxCoderAddByte(param, 0, XS_CODE_START_ASYNC);

	coder->returnTarget = fxCoderCreateTarget(param);
	
	txUsingContext context;
	if (self->scope->disposableNodeCount)
		fxScopeCodeUsing(self->scope, coder, &context);
	fxNodeDispatchCode(self->body, param);
	if (self->scope->disposableNodeCount)
		fxScopeCodeUsed(self->scope, coder, &context);
	
	fxCoderAdd(param, 0, coder->returnTarget);
	fxCoderAddByte(param, 0, XS_CODE_END);
	fxCoderAdd(param, 0, target);
	
	fxCoderAddByte(param, 1, XS_CODE_ENVIRONMENT);
	fxCoderAddByte(param, -1, XS_CODE_POP);
	
	count = 2 + fxScopeCodeSpecifierNodes(self->scope, coder);
	fxCoderAddInteger(coder, 1, XS_CODE_INTEGER_1, count);
	if (!(self->flags & mxStrictFlag))
		flag |= XS_JSON_MODULE_FLAG;
	if (coder->importFlag)
		flag |= XS_IMPORT_FLAG;
	if (coder->importMetaFlag)
		flag |= XS_IMPORT_META_FLAG;
	fxCoderAddIndex(coder, 0 - count, XS_CODE_MODULE, flag);
	fxCoderAddByte(coder, -1, XS_CODE_SET_RESULT);
	fxCoderAddByte(coder, 0, XS_CODE_END);
}

void fxNewNodeCode(void* it, void* param) 
{
	txCallNewNode* self = it;
	fxNodeDispatchCode(self->reference, param);
	fxCoderAddByte(param, 2, XS_CODE_NEW);
	fxNodeDispatchCode(self->params, param);
}

void fxNumberNodeCode(void* it, void* param) 
{
	txNumberNode* self = it;
	fxCoderAddNumber(param, 1, XS_CODE_NUMBER, self->value);
}

void fxObjectNodeCode(void* it, void* param) 
{
	txObjectNode* self = it;
	txCoder* coder = param;
	txInteger object = fxCoderUseTemporaryVariable(param);
	txNode* item;
	txFlag flag = 0;
	if (self->items) {
		item = self->items->first;
		while (item) {
			if (item->description->token == XS_TOKEN_PROPERTY) {
				if (!(item->flags & mxShorthandFlag) && (((txPropertyNode*)item)->symbol == coder->parser->__proto__Symbol)) {
					if (flag)
						fxReportParserError(coder->parser, item->line, "invalid __proto__");
					flag = 1;
					fxNodeDispatchCode(((txPropertyNode*)item)->value, param);
					fxCoderAddByte(param, 0, XS_CODE_INSTANTIATE);
				}
			}
			item = item->next;
		}
	}
	if (!flag)
		fxCoderAddByte(param, 1, XS_CODE_OBJECT);
	fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, object);
	if (self->items) {
		item = self->items->first;
		while (item) {
			if (item->description->token == XS_TOKEN_SPREAD) {
				fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
				fxCoderAddByte(param, 1, XS_CODE_COPY_OBJECT);
				fxCoderAddByte(param, 1, XS_CODE_CALL);
				fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, object);
				fxNodeDispatchCode(((txSpreadNode*)item)->expression, param);
				fxCoderAddInteger(param, -4, XS_CODE_RUN_1, 2);
				fxCoderAddByte(param, -1, XS_CODE_POP);
			}
			else {
				txNode* value;
				if (item->description->token == XS_TOKEN_PROPERTY) {
					if (!(item->flags & mxShorthandFlag) && (((txPropertyNode*)item)->symbol == coder->parser->__proto__Symbol)) {
						item = item->next;
						continue;
					}
					value = ((txPropertyNode*)item)->value;
					fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, object);
					fxNodeDispatchCode(value, param);
					fxCoderAddSymbol(param, -2, XS_CODE_NEW_PROPERTY, ((txPropertyNode*)item)->symbol);
				}
				else {
					value = ((txPropertyAtNode*)item)->value;
					fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, object);
					fxNodeDispatchCode(((txPropertyAtNode*)item)->at, param);
					fxCoderAddByte(param, 0, XS_CODE_AT);
					fxNodeDispatchCode(value, param);
					fxCoderAddByte(param, -3, XS_CODE_NEW_PROPERTY_AT);
				}
				flag = 0;
				if (item->flags & mxMethodFlag)
					flag |= XS_NAME_FLAG | XS_METHOD_FLAG;
				else if (item->flags & mxGetterFlag)
					flag |= XS_NAME_FLAG | XS_METHOD_FLAG | XS_GETTER_FLAG;
				else if (item->flags & mxSetterFlag)
					flag |= XS_NAME_FLAG | XS_METHOD_FLAG | XS_SETTER_FLAG;
				else if (fxNodeCodeName(value))
					flag |= XS_NAME_FLAG;
				fxCoderAddInteger(param, 0, XS_CODE_INTEGER_1, flag);
			}
			item = item->next;
		}
	}
	fxCoderUnuseTemporaryVariables(param, 1);
}

void fxObjectBindingNodeCode(void* it, void* param)
{
	txObjectBindingNode* self = it;
	txCoder* coder = param;
	fxCoderAddByte(coder, 1, XS_CODE_UNDEFINED);
	fxObjectBindingNodeCodeAssign(self, param, 0);
}

void fxObjectBindingNodeCodeAssign(void* it, void* param, txFlag flag) 
{
	txObjectBindingNode* self = it;
	txNode* item = self->items->first;
	txInteger object;
	txInteger at;
	txInteger c = 0;
	object = fxCoderUseTemporaryVariable(param);
	at = fxCoderUseTemporaryVariable(param);
	fxCoderAddByte(param, 1, XS_CODE_DUB);
	fxCoderAddByte(param, 0, XS_CODE_TO_INSTANCE);
	fxCoderAddIndex(param, -1, XS_CODE_PULL_LOCAL_1, object);
	if (self->flags & mxSpreadFlag) {
		fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
		fxCoderAddByte(param, 1, XS_CODE_COPY_OBJECT);
		fxCoderAddByte(param, 1, XS_CODE_CALL);
		fxCoderAddByte(param, 1, XS_CODE_OBJECT);
		fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, object);
		c = 2;
	}
	while (item && (item->description->token != XS_TOKEN_REST_BINDING)) {
		if (item->description->token == XS_TOKEN_PROPERTY_BINDING) {
			if (self->flags & mxSpreadFlag) {
				fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, object);
				fxCoderAddSymbol(param, 1, XS_CODE_SYMBOL, ((txPropertyBindingNode*)item)->symbol);
				fxCoderAddByte(param, 0, XS_CODE_AT);
				fxCoderAddByte(param, 0, XS_CODE_SWAP);
				fxCoderAddByte(param, -1, XS_CODE_POP);
				c++;
			}
			fxNodeDispatchCodeReference(((txPropertyBindingNode*)item)->binding, param, 1);
			fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, object);
			fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, ((txPropertyBindingNode*)item)->symbol);
			fxNodeDispatchCodeAssign(((txPropertyBindingNode*)item)->binding, param, 1);
		}
		else {
			fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, object);
			fxNodeDispatchCode(((txPropertyBindingAtNode*)item)->at, param);
			fxCoderAddByte(param, 0, XS_CODE_AT);
			if (self->flags & mxSpreadFlag) {
				fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, at);
				fxCoderAddByte(param, 0, XS_CODE_SWAP);
				fxCoderAddByte(param, -1, XS_CODE_POP);
				c++;
			}
			else {
				fxCoderAddIndex(param, -1, XS_CODE_PULL_LOCAL_1, at);
				fxCoderAddByte(param, -1, XS_CODE_POP);
			}
			fxNodeDispatchCodeReference(((txPropertyBindingAtNode*)item)->binding, param, 1);
			fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, object);
			fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, at);
			fxCoderAddByte(param, -1, XS_CODE_GET_PROPERTY_AT);
			fxNodeDispatchCodeAssign(((txPropertyBindingAtNode*)item)->binding, param, 1);
		}
		fxCoderAddByte(param, -1, XS_CODE_POP);
		item = item->next;
	}
	if (self->flags & mxSpreadFlag) {
		fxCoderAddInteger(param, -2 - c, XS_CODE_RUN_1, c);
		fxCoderAddIndex(param, -1, XS_CODE_PULL_LOCAL_1, object);
		fxNodeDispatchCodeReference(((txRestBindingNode*)item)->binding, param, 1);
		fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, object);
		fxNodeDispatchCodeAssign(((txRestBindingNode*)item)->binding, param, 1);
        fxCoderAddByte(param, -1, XS_CODE_POP);
	}
	fxCoderUnuseTemporaryVariables(param, 2);
}

void fxOptionNodeCode(void* it, void* param) 
{
	txUnaryExpressionNode* self = it;
	txCoder* coder = param;
	self->right->flags |= (self->flags & mxTailRecursionFlag);
	fxNodeDispatchCode(self->right, param);
	fxCoderAddBranch(param, 0, XS_CODE_BRANCH_CHAIN_1, coder->chainTarget);
}

txFlag fxOptionNodeCodeThis(void* it, void* param, txFlag flag) 
{
	txUnaryExpressionNode* self = it;
	txCoder* coder = param;
	txTargetCode* swapTarget = fxCoderCreateTarget(param);
	txTargetCode* skipTarget = fxCoderCreateTarget(param);
	self->right->flags |= (self->flags & mxTailRecursionFlag);
	flag = fxNodeDispatchCodeThis(self->right, param, flag);
	fxCoderAddBranch(param, 0, XS_CODE_BRANCH_CHAIN_1, swapTarget);
	fxCoderAddBranch(param, 1, XS_CODE_BRANCH_1, skipTarget);
	fxCoderAdd(param, 0, swapTarget);
	fxCoderAddByte(param, 0, XS_CODE_SWAP);
	fxCoderAddByte(param, -1, XS_CODE_POP);
	fxCoderAddBranch(param, 0, XS_CODE_BRANCH_1, coder->chainTarget);
	fxCoderAdd(param, 0, skipTarget);
	return flag;
}

void fxOrExpressionNodeCode(void* it, void* param) 
{
	txBinaryExpressionNode* self = it;
	txTargetCode* endTarget = fxCoderCreateTarget(param);
	self->right->flags |= (self->flags & mxTailRecursionFlag);
	fxNodeDispatchCode(self->left, param);
	fxCoderAddByte(param, 1, XS_CODE_DUB);
	fxCoderAddBranch(param, -1, XS_CODE_BRANCH_IF_1, endTarget);
	fxCoderAddByte(param, -1, XS_CODE_POP);
	fxNodeDispatchCode(self->right, param);
	fxCoderAdd(param, 0, endTarget);
}

void fxParamsNodeCode(void* it, void* param) 
{
	txParamsNode* self = it;
	txInteger c = 0;
	if (self->flags & mxSpreadFlag) {
		txInteger counter = fxCoderUseTemporaryVariable(param);
		fxCoderAddInteger(param, 1, XS_CODE_INTEGER_1, 0);
		fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, counter);
		fxCoderAddByte(param, -1, XS_CODE_POP);
		if (self->items) {
			txNode* item = self->items->first;
			while (item) {
				if (item->description->token == XS_TOKEN_SPREAD) {
					fxSpreadNodeCode(item, param, counter);
				}
				else {
					c++;
					fxNodeDispatchCode(item, param);
					fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, counter);
					fxCoderAddInteger(param, 1, XS_CODE_INTEGER_1, 1);
					fxCoderAddByte(param, -1, XS_CODE_ADD);
					fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, counter);
					fxCoderAddByte(param, -1, XS_CODE_POP);
				}
				item = item->next;
			}
		}
		fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, counter);
		if (self->flags & mxEvalParametersFlag)
			fxCoderAddByte(param, -3 - c, (self->flags & mxTailRecursionFlag) ? XS_CODE_EVAL_TAIL : XS_CODE_EVAL);
		else
			fxCoderAddByte(param, -3 - c, (self->flags & mxTailRecursionFlag) ? XS_CODE_RUN_TAIL : XS_CODE_RUN);
		fxCoderUnuseTemporaryVariables(param, 1);
	}
	else {
		if (self->items) {
			txNode* item = self->items->first;
			while (item) {
				fxNodeDispatchCode(item, param);
				c++;
				item = item->next;
			}
			if (self->flags & mxEvalParametersFlag) {
				fxCoderAddInteger(param, 1, XS_CODE_INTEGER_1, c);
				fxCoderAddByte(param, -3 - c, (self->flags & mxTailRecursionFlag) ? XS_CODE_EVAL_TAIL : XS_CODE_EVAL);
			}
			else
				fxCoderAddInteger(param, -2 - c, (self->flags & mxTailRecursionFlag) ? XS_CODE_RUN_TAIL_1 : XS_CODE_RUN_1, c);
		}
	}
}

void fxParamsBindingNodeCode(void* it, void* param) 
{
	txParamsBindingNode* self = it;
	txNode* item = self->items->first;
	txInteger index = 0;
	if (self->declaration) {
		if (self->mapped)
			fxCoderAddIndex(param, 1, XS_CODE_ARGUMENTS_SLOPPY, self->items->length);
		else
			fxCoderAddIndex(param, 1, XS_CODE_ARGUMENTS_STRICT, self->items->length);
		if (self->declaration->flags & mxDeclareNodeClosureFlag)
			fxCoderAddIndex(param, 0, XS_CODE_VAR_CLOSURE_1, self->declaration->index);
		else 
			fxCoderAddIndex(param, 0, XS_CODE_VAR_LOCAL_1, self->declaration->index);
		fxCoderAddByte(param, -1, XS_CODE_POP);
	}
	while (item) {
		if (item->description->token == XS_TOKEN_REST_BINDING) {
			fxNodeDispatchCodeReference(((txRestBindingNode*)item)->binding, param, 0);
			fxCoderAddIndex(param, 1, XS_CODE_ARGUMENTS, index);
			fxNodeDispatchCodeAssign(((txRestBindingNode*)item)->binding, param, 0);
		}
		else {
			fxNodeDispatchCodeReference(item, param, 0);
			fxCoderAddIndex(param, 1, XS_CODE_ARGUMENT, index);
			fxNodeDispatchCodeAssign(item, param, 0);
		}
		fxCoderAddByte(param, -1, XS_CODE_POP);
		item = item->next;
		index++;
	}
}

void fxPostfixExpressionNodeCode(void* it, void* param) 
{
	txPostfixExpressionNode* self = it;
	txInteger value = 0;
	fxNodeDispatchCodeThis(self->left, param, 1);
	if (!(self->flags & mxExpressionNoValue)) {
		value = fxCoderUseTemporaryVariable(param);
		fxCoderAddByte(param, 0, XS_CODE_TO_NUMERIC);
		fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, value);
	}
	fxCoderAddByte(param, 0, self->description->code);
	fxNodeDispatchCodeAssign(self->left, param, 0);
	if (!(self->flags & mxExpressionNoValue)) {
		fxCoderAddByte(param, -1, XS_CODE_POP);
		fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, value);
		fxCoderUnuseTemporaryVariables(param, 1);
	}
}

void fxPrivateIdentifierNodeCode(void* it, void* param) 
{
	txPrivateMemberNode* self = it;
	fxNodeDispatchCode(self->reference, param);
	fxCoderAddIndex(param, 0,  XS_CODE_HAS_PRIVATE_1, self->declaration->index);
}

void fxPrivateMemberNodeCode(void* it, void* param) 
{
	txPrivateMemberNode* self = it;
	fxNodeDispatchCode(self->reference, param);
	fxCoderAddIndex(param, 0,  XS_CODE_GET_PRIVATE_1, self->declaration->index);
}

void fxPrivateMemberNodeCodeAssign(void* it, void* param, txFlag flag) 
{
	txPrivateMemberNode* self = it;
	fxCoderAddIndex(param, -1, XS_CODE_SET_PRIVATE_1, self->declaration->index);
}

void fxPrivateMemberNodeCodeDelete(void* it, void* param) 
{
	txPrivateMemberNode* self = it;
	txCoder* coder = param;
	fxNodeDispatchCode(self->reference, param);
	fxReportParserError(coder->parser, self->line, "delete private property");
}

void fxPrivateMemberNodeCodeReference(void* it, void* param, txFlag flag) 
{
	txPrivateMemberNode* self = it;
	fxNodeDispatchCode(self->reference, param);
}

txFlag fxPrivateMemberNodeCodeThis(void* it, void* param, txFlag flag) 
{
	txPrivateMemberNode* self = it;
	fxNodeDispatchCode(self->reference, param);
	fxCoderAddByte(param, 1, XS_CODE_DUB);
	fxCoderAddIndex(param, 0, XS_CODE_GET_PRIVATE_1, self->declaration->index);
	return 1;
}

void fxProgramNodeCode(void* it, void* param) 
{
	txProgramNode* self = it;
	txCoder* coder = param;
	XS_NR_ROOT_SHIM(fxProgramNodeCode, WC_PROGRAM)
	
	coder->line = kNoLine;
	coder->programFlag = 1;
	coder->scopeLevel = 0;
	coder->firstBreakTarget = NULL;
	coder->firstContinueTarget = NULL;
	
	if (self->flags & mxStrictFlag)
		fxCoderAddIndex(param, 0, XS_CODE_BEGIN_STRICT, 0);
	else
		fxCoderAddIndex(param, 0, XS_CODE_BEGIN_SLOPPY, 0);
	coder->path = C_NULL;
	if (self->line != kNoLine)
		fxCoderAddLine(coder, 0, XS_CODE_LINE, it); 
	if (coder->parser->flags & mxEvalFlag) {
		coder->evalFlag = 1;
		fxScopeCodingEval(self->scope, param);
	}
	else
		fxScopeCodingProgram(self->scope, param);
	coder->returnTarget = fxCoderCreateTarget(param);
	fxScopeCodeDefineNodes(self->scope, param);
	fxNodeDispatchCode(self->body, param);
	fxCoderAdd(param, 0, coder->returnTarget);
	fxCoderAddByte(param, 0, XS_CODE_RETURN);
}

void fxQuestionMarkNodeCode(void* it, void* param) 
{
	txQuestionMarkNode* self = it;
	txTargetCode* elseTarget = fxCoderCreateTarget(param);
	txTargetCode* endTarget = fxCoderCreateTarget(param);
	self->thenExpression->flags |= (self->flags & mxTailRecursionFlag);
	self->elseExpression->flags |= (self->flags & mxTailRecursionFlag);
	fxNodeDispatchCode(self->expression, param);
	fxCoderAddBranch(param, -1, XS_CODE_BRANCH_ELSE_1, elseTarget);
	fxNodeDispatchCode(self->thenExpression, param);
	fxCoderAddBranch(param, 0, XS_CODE_BRANCH_1, endTarget);
	fxCoderAdd(param, -1, elseTarget);
	fxNodeDispatchCode(self->elseExpression, param);
	fxCoderAdd(param, 0, endTarget);
}

void fxRegexpNodeCode(void* it, void* param) 
{
	txRegexpNode* self = it;
	fxCoderAddByte(param, 1, XS_CODE_REGEXP);
	fxCoderAddByte(param, 2, XS_CODE_NEW);
	fxNodeDispatchCode(self->modifier, param);
	fxNodeDispatchCode(self->value, param);
	fxCoderAddInteger(param, -4, XS_CODE_RUN_1, 2);
}

void fxReturnNodeCode(void* it, void* param) 
{
	txStatementNode* self = it;
	txCoder* coder = param;
	if (coder->programFlag)
		fxReportParserError(coder->parser, self->line, "invalid return");
	if (self->expression) {	
		if (((self->flags & (mxStrictFlag | mxGeneratorFlag)) == mxStrictFlag) && (coder->returnTarget->original == NULL))
			self->expression->flags |= mxTailRecursionFlag;
		fxNodeDispatchCode(self->expression, param);
		if ((self->flags & (mxAsyncFlag | mxGeneratorFlag)) == (mxAsyncFlag | mxGeneratorFlag)) {
			fxCoderAddByte(param, 0, XS_CODE_AWAIT);
			fxCoderAddByte(coder, 0, XS_CODE_THROW_STATUS);
		}
		fxCoderAddByte(param, -1, XS_CODE_SET_RESULT);
	}
	else if ((self->flags & (mxAsyncFlag | mxGeneratorFlag)) != (mxAsyncFlag | mxGeneratorFlag)) {
		fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
		fxCoderAddByte(param, -1, XS_CODE_SET_RESULT);
	}
	fxCoderAdjustEnvironment(coder, coder->returnTarget);
	fxCoderAdjustScope(coder, coder->returnTarget);
	fxCoderAddBranch(param, 0, XS_CODE_BRANCH_1, coder->returnTarget);
}

void fxSpreadNodeCode(void* it, void* param, txInteger counter) 
{
	txSpreadNode* self = it;
	txCoder* coder = param;
	txTargetCode* nextTarget = fxCoderCreateTarget(param);
	txTargetCode* doneTarget = fxCoderCreateTarget(param);
	txInteger iterator;
	fxNodeDispatchCode(self->expression, param);
	fxCoderAddByte(param, 0, XS_CODE_FOR_OF);
	iterator = fxCoderUseTemporaryVariable(param);
	fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, iterator);
	fxCoderAddByte(param, -1, XS_CODE_POP);
	fxCoderAdd(param, 0, nextTarget);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, iterator);
	fxCoderAddByte(param, 1, XS_CODE_DUB);
	fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->nextSymbol);
	fxCoderAddByte(param, 1, XS_CODE_CALL);
	fxCoderAddInteger(param, -2, XS_CODE_RUN_1, 0);
	fxCoderAddByte(param, 1, XS_CODE_DUB);
	fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->doneSymbol);
	fxCoderAddBranch(param, -1, XS_CODE_BRANCH_IF_1, doneTarget);
	fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->valueSymbol);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, counter);
	fxCoderAddInteger(param, 1, XS_CODE_INTEGER_1, 1);
	fxCoderAddByte(param, -1, XS_CODE_ADD);
	fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, counter);
	fxCoderAddByte(param, -1, XS_CODE_POP);
	fxCoderAddBranch(param, 0, XS_CODE_BRANCH_1, nextTarget);
	fxCoderAdd(param, 1, doneTarget);
	fxCoderAddByte(param, -1, XS_CODE_POP);
	fxCoderUnuseTemporaryVariables(param, 1);
}

void fxStatementNodeCode(void* it, void* param) 
{
	txStatementNode* self = it;
	txCoder* coder = param;
	if (coder->programFlag) {
		fxNodeDispatchCode(self->expression, param);
		fxCoderAddByte(param, -1, XS_CODE_SET_RESULT);
	}
	else {
		self->expression->flags |= mxExpressionNoValue;
		fxNodeDispatchCode(self->expression, param);
		if (coder->lastCode->id == XS_CODE_SET_CLOSURE_1) {
			coder->lastCode->id = XS_CODE_PULL_CLOSURE_1;
			coder->stackLevel--;
			coder->lastCode->stackLevel = coder->stackLevel;
		}
		else if (coder->lastCode->id == XS_CODE_SET_LOCAL_1) {
			coder->lastCode->id = XS_CODE_PULL_LOCAL_1;
			coder->stackLevel--;
			coder->lastCode->stackLevel = coder->stackLevel;
		}
		else
			fxCoderAddByte(param, -1, XS_CODE_POP);
	}
}

void fxStatementsNodeCode(void* it, void* param) 
{
	txStatementsNode* self = it;
	txNode* item = self->items->first;
	while (item) {
		fxNodeDispatchCode(item, param);
		item = item->next;
	}
}

void fxStringNodeCode(void* it, void* param) 
{
	txStringNode* self = it;
	txCoder* coder = param;
	txParser* parser = coder->parser;
	if (self->flags & mxStringErrorFlag)
		fxReportParserError(parser, self->line, "invalid escape sequence");
	fxCoderAddString(param, 1, XS_CODE_STRING_1, self->length, self->value);
}

void fxSuperNodeCode(void* it, void* param)
{
	txSuperNode* self = it;
	txCoder* coder = param;
	if (coder->classNode->heritage->description->token == XS_TOKEN_HOST) {
		fxCoderAddByte(param, 1, XS_CODE_TARGET);
		fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, coder->parser->prototypeSymbol);
		fxCoderAddByte(param, 0, XS_CODE_INSTANTIATE);
	}
	else {
		fxCoderAddByte(param, 3, XS_CODE_SUPER);
		fxNodeDispatchCode(self->params, param);
	}
	fxCoderAddByte(param, 0, XS_CODE_SET_THIS);
	if (self->instanceInitAccess) {
		fxCoderAddByte(param, 1, XS_CODE_GET_THIS);
		fxCoderAddIndex(param, 1, XS_CODE_GET_CLOSURE_1, self->instanceInitAccess->declaration->index);
		fxCoderAddByte(param, 1, XS_CODE_CALL);
		fxCoderAddInteger(param, -2, XS_CODE_RUN_1, 0);
		fxCoderAddByte(param, -1, XS_CODE_POP);
	}
}

void fxSwitchNodeCode(void* it, void* param) 
{
	txSwitchNode* self = it;
	txCoder* coder = param;
	txTargetCode* breakTarget;
	txCaseNode* caseNode;
	txCaseNode* defaultNode = NULL;
	txUsingContext context;
	fxNodeDispatchCode(self->expression, param);
	fxScopeCodingBlock(self->scope, param);
	if (self->scope->disposableNodeCount)
		fxScopeCodeUsing(self->scope, coder, &context);
	breakTarget = fxCoderCreateTarget(coder);
	breakTarget->label = fxNewParserChunkClear(coder->parser, sizeof(txLabelNode));
	breakTarget->nextTarget = coder->firstBreakTarget;
	coder->firstBreakTarget = breakTarget;
	if (coder->programFlag) {
		fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
		fxCoderAddByte(param, -1, XS_CODE_SET_RESULT);
	}
	caseNode = (txCaseNode*)self->items->first;
	while (caseNode) {
		caseNode->target = fxCoderCreateTarget(param);
		if (caseNode->expression) {
			fxCoderAddByte(param, 1, XS_CODE_DUB);
			fxNodeDispatchCode(caseNode->expression, param);
			fxCoderAddByte(param, -1, XS_CODE_STRICT_EQUAL);
			fxCoderAddBranch(param, -1, XS_CODE_BRANCH_IF_1, caseNode->target);
		}
		else
			defaultNode = caseNode;
		caseNode = (txCaseNode*)caseNode->next;
	}
	if (defaultNode)
		fxCoderAddBranch(param, 0, XS_CODE_BRANCH_1, defaultNode->target);
	else
		fxCoderAddBranch(param, 0, XS_CODE_BRANCH_1, coder->firstBreakTarget);
	caseNode = (txCaseNode*)self->items->first;
	while (caseNode) {
		fxCoderAdd(param, 0, caseNode->target);
		if (caseNode->statement)
			fxNodeDispatchCode(caseNode->statement, param);
		caseNode = (txCaseNode*)caseNode->next;
	}
	fxCoderAdd(param, 0, coder->firstBreakTarget);
	coder->firstBreakTarget = breakTarget->nextTarget;
	if (self->scope->disposableNodeCount)
		fxScopeCodeUsed(self->scope, coder, &context);
	fxScopeCoded(self->scope, param);
	fxCoderAddByte(param, -1, XS_CODE_POP);
}

void fxTemplateNodeCode(void* it, void* param) 
{
	txTemplateNode* self = it;
	txCoder* coder = param;
	txParser* parser = coder->parser;
	txNode* item = self->items->first;
	
	if (self->reference) {
		txSymbol* symbol;
		txTargetCode* cacheTarget = fxCoderCreateTarget(param);
		txInteger i = (self->items->length / 2) + 1;
		txInteger raws = fxCoderUseTemporaryVariable(param);
		txInteger strings = fxCoderUseTemporaryVariable(param);
		txFlag flag = XS_DONT_DELETE_FLAG | XS_DONT_SET_FLAG;

		fxNodeDispatchCodeThis(self->reference, param, 0);
		fxCoderAddByte(param, 1, XS_CODE_CALL);

		fxGenerateTag(parser->console, parser->buffer, parser->bufferSize, (parser->path) ? parser->path->string : C_NULL);
		symbol = fxNewParserSymbol(parser, parser->buffer);
		fxCoderAddByte(param, 1, XS_CODE_TEMPLATE_CACHE);
		fxCoderAddSymbol(param, 0, XS_CODE_GET_PROPERTY, symbol);
		fxCoderAddBranch(param, 0, XS_CODE_BRANCH_COALESCE_1, cacheTarget);
		fxCoderAddByte(param, 1, XS_CODE_TEMPLATE_CACHE);

		fxCoderAddByte(param, 1, XS_CODE_ARRAY);
		fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, strings);
		fxCoderAddInteger(param, 1, XS_CODE_INTEGER_1, i);
		fxCoderAddSymbol(param, -1, XS_CODE_SET_PROPERTY, coder->parser->lengthSymbol);
		fxCoderAddByte(param, -1, XS_CODE_POP);
		fxCoderAddByte(param, 1, XS_CODE_ARRAY);
		fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, raws);
		fxCoderAddInteger(param, 1, XS_CODE_INTEGER_1, i);
		fxCoderAddSymbol(param, -1, XS_CODE_SET_PROPERTY, coder->parser->lengthSymbol);
		fxCoderAddByte(param, -1, XS_CODE_POP);
		i = 0;
		while (item) {
			if (item->description->token == XS_TOKEN_TEMPLATE_MIDDLE) {
		
				fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, strings);
				fxCoderAddInteger(param, 1, XS_CODE_INTEGER_1, i);
				fxCoderAddByte(param, 0, XS_CODE_AT);
				if (((txTemplateItemNode*)item)->string->flags & mxStringErrorFlag)
					fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
				else
					fxNodeDispatchCode(((txTemplateItemNode*)item)->string, param);
				fxCoderAddByte(param, -3, XS_CODE_NEW_PROPERTY_AT);
				fxCoderAddInteger(param, 0, XS_CODE_INTEGER_1, flag);
			
				fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, raws);
				fxCoderAddInteger(param, 1, XS_CODE_INTEGER_1, i);
				fxCoderAddByte(param, 0, XS_CODE_AT);
				fxNodeDispatchCode(((txTemplateItemNode*)item)->raw, param);
				fxCoderAddByte(param, -3, XS_CODE_NEW_PROPERTY_AT);
				fxCoderAddInteger(param, 0, XS_CODE_INTEGER_1, flag);
			
				i++;
			}
			item = item->next;
		}
		fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, strings);
		fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, raws);
		fxCoderAddSymbol(param, -1, XS_CODE_SET_PROPERTY, parser->rawSymbol);
		fxCoderAddByte(param, -1, XS_CODE_POP);
		fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, strings);
		fxCoderAddByte(param, 0, XS_CODE_TEMPLATE);
		
		fxCoderAddSymbol(param, -1, XS_CODE_SET_PROPERTY, symbol);
		
		fxCoderAdd(param, 0, cacheTarget);
		
		i = 1;
		item = self->items->first;
		while (item) {
			if (item->description->token != XS_TOKEN_TEMPLATE_MIDDLE) {
				fxNodeDispatchCode(item, param);
				i++;
			}
			item = item->next;
		}
		fxCoderAddInteger(param, -2 - i, (self->flags & mxTailRecursionFlag) ? XS_CODE_RUN_TAIL_1 : XS_CODE_RUN_1, i);
		fxCoderUnuseTemporaryVariables(coder, 2);
	}
	else {
		fxNodeDispatchCode(((txTemplateItemNode*)item)->string, param);
		item = item->next;
		while (item) {
			if (item->description->token == XS_TOKEN_TEMPLATE_MIDDLE) {
				fxNodeDispatchCode(((txTemplateItemNode*)item)->string, param);
			}
			else {
				fxNodeDispatchCode(item, param);
				fxCoderAddByte(param, 1, XS_CODE_TO_STRING);
			}
			fxCoderAddByte(param, -1, XS_CODE_ADD);
			item = item->next;
		}
	}
}

void fxThisNodeCode(void* it, void* param) 
{
	txNode* self = it;
	if (self->flags & mxDerivedFlag)
		fxCoderAddByte(param, 1, XS_CODE_GET_THIS);
	else
		fxCoderAddByte(param, 1, self->description->code);
}

void fxThrowNodeCode(void* it, void* param) 
{
	txStatementNode* self = it;
	fxNodeDispatchCode(self->expression, param);
	fxCoderAddByte(param, -1, XS_CODE_THROW);
}

void fxTryNodeCode(void* it, void* param) 
{
	txTryNode* self = it;
	txCoder* coder = param;
	txInteger exception;
	txInteger selector;
	txInteger result;
	txInteger selection;
	txTargetCode* catchTarget;
	txTargetCode* normalTarget;
	txTargetCode* finallyTarget;

	exception = fxCoderUseTemporaryVariable(coder);
	selector = fxCoderUseTemporaryVariable(coder);
	result = fxCoderUseTemporaryVariable(coder);

	coder->firstBreakTarget = fxCoderAliasTargets(param, coder->firstBreakTarget);
	coder->firstContinueTarget = fxCoderAliasTargets(param, coder->firstContinueTarget);
	coder->returnTarget = fxCoderAliasTargets(param, coder->returnTarget);
	catchTarget = fxCoderCreateTarget(param);
	normalTarget = fxCoderCreateTarget(param);
	finallyTarget = fxCoderCreateTarget(param);

	fxCoderAddInteger(param, 1, XS_CODE_INTEGER_1, 0);
	fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, selector);
	fxCoderAddByte(param, -1, XS_CODE_POP);

	fxCoderAddBranch(param, 0, XS_CODE_CATCH_1, catchTarget);
	if (coder->programFlag) {
		fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
		fxCoderAddByte(param, -1, XS_CODE_SET_RESULT);
	}
	fxNodeDispatchCode(self->tryBlock, param);
	fxCoderAddBranch(param, 0, XS_CODE_BRANCH_1, normalTarget);
	if (self->catchBlock) {
		fxCoderAddByte(param, 0, XS_CODE_UNCATCH);
		fxCoderAdd(param, 0, catchTarget);
		catchTarget = fxCoderCreateTarget(param);
		fxCoderAddBranch(param, 0, XS_CODE_CATCH_1, catchTarget);
		if (coder->programFlag) {
			fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
			fxCoderAddByte(param, -1, XS_CODE_SET_RESULT);
		}
		fxNodeDispatchCode(self->catchBlock, param);
		fxCoderAddBranch(param, 0, XS_CODE_BRANCH_1, normalTarget);
	}
	
	selection = 1;
	coder->firstBreakTarget = fxCoderFinalizeTargets(param, coder->firstBreakTarget, selector, &selection, finallyTarget);
	coder->firstContinueTarget = fxCoderFinalizeTargets(param, coder->firstContinueTarget, selector, &selection, finallyTarget);
	coder->returnTarget = fxCoderFinalizeTargets(param, coder->returnTarget, selector, &selection, finallyTarget);
	fxCoderAdd(param, 0, normalTarget);
	fxCoderAddInteger(param, 1, XS_CODE_INTEGER_1, selection);
	fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, selector);
	fxCoderAddByte(param, -1, XS_CODE_POP);
	fxCoderAdd(param, 0, finallyTarget);
	fxCoderAddByte(param, 0, XS_CODE_UNCATCH);
	fxCoderAdd(param, 0, catchTarget);
	fxCoderAddByte(param, 1, XS_CODE_EXCEPTION);
	fxCoderAddIndex(param, 0, XS_CODE_SET_LOCAL_1, exception);
	fxCoderAddByte(param, -1, XS_CODE_POP);
	if (self->finallyBlock) {
		if (coder->programFlag) {
			fxCoderAddByte(param, 1, XS_CODE_GET_RESULT);
			fxCoderAddIndex(param, -1, XS_CODE_PULL_LOCAL_1, result);
			fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
			fxCoderAddByte(param, -1, XS_CODE_SET_RESULT);
		}
		fxNodeDispatchCode(self->finallyBlock, param);
		if (coder->programFlag) {
			fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, result);
			fxCoderAddByte(param, -1, XS_CODE_SET_RESULT);
		}		
	}
	catchTarget = fxCoderCreateTarget(param);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, selector);
	fxCoderAddBranch(param, -1, XS_CODE_BRANCH_IF_1, catchTarget);
	fxCoderAddIndex(param, 1, XS_CODE_GET_LOCAL_1, exception);
	fxCoderAddByte(param, -1, XS_CODE_THROW);
	fxCoderAdd(param, 0, catchTarget);
	selection = 1;
	fxCoderJumpTargets(param, coder->firstBreakTarget, selector, &selection);
	fxCoderJumpTargets(param, coder->firstContinueTarget, selector, &selection);
	fxCoderJumpTargets(param, coder->returnTarget, selector, &selection);
	fxCoderUnuseTemporaryVariables(coder, 3);
}

void fxUnaryExpressionNodeCode(void* it, void* param) 
{
	txUnaryExpressionNode* self = it;
	fxNodeDispatchCode(self->right, param);
	fxCoderAddByte(param, 0, self->description->code);
}

void fxUndefinedNodeCodeAssign(void* it, void* param, txFlag flag) 
{
	txCoder* coder = param;
	fxCoderAddSymbol(param, -1, XS_CODE_SET_VARIABLE, coder->parser->undefinedSymbol);
}

void fxUndefinedNodeCodeDelete(void* it, void* param) 
{
	txNode* self = it;
	txCoder* coder = param;
	if (self->flags & mxStrictFlag)
		fxReportParserError(coder->parser, self->line, "delete identifier (strict code)");
	fxCoderAddByte(param, 1, XS_CODE_FALSE);
}

void fxUndefinedNodeCodeReference(void* it, void* param, txFlag flag) 
{
	txCoder* coder = param;
	if (coder->evalFlag)
		fxCoderAddSymbol(param, 1, XS_CODE_EVAL_REFERENCE, coder->parser->undefinedSymbol);
	else
		fxCoderAddSymbol(param, 1, XS_CODE_PROGRAM_REFERENCE, coder->parser->undefinedSymbol);
}

void fxValueNodeCode(void* it, void* param) 
{
	txNode* self = it;
	fxCoderAddByte(param, 1, self->description->code);
}

void fxWhileNodeCode(void* it, void* param) 
{
	txWhileNode* self = it;
	txCoder* coder = param;
	if (coder->programFlag) {
		fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
		fxCoderAddByte(param, -1, XS_CODE_SET_RESULT);
	}
	fxCoderAdd(param, 0, coder->firstContinueTarget);
	fxNodeDispatchCode(self->expression, param);
	fxCoderAddBranch(param, -1, XS_CODE_BRANCH_ELSE_1, coder->firstBreakTarget);
	fxNodeDispatchCode(self->statement, param);
	fxCoderAddBranch(param, 0, XS_CODE_BRANCH_1, coder->firstContinueTarget);
}

void fxWithNodeCode(void* it, void* param)
{
	txWithNode* self = it;
	txCoder* coder = param;
	txBoolean evalFlag;
	fxNodeDispatchCode(self->expression, param);
	fxCoderAddByte(param, 0, XS_CODE_TO_INSTANCE);
	fxCoderAddByte(param, 0, XS_CODE_WITH);
	fxCoderAddByte(param, -1, XS_CODE_POP);
	evalFlag = coder->evalFlag;
	coder->environmentLevel++;
	coder->evalFlag = 1;
	if (coder->programFlag) {
		fxCoderAddByte(param, 1, XS_CODE_UNDEFINED);
		fxCoderAddByte(param, -1, XS_CODE_SET_RESULT);
	}
	fxNodeDispatchCode(self->statement, param);
	coder->evalFlag = evalFlag;
	coder->environmentLevel--;
	fxCoderAddByte(param, 0, XS_CODE_WITHOUT);
}

void fxYieldNodeCode(void* it, void* param) 
{
	txStatementNode* self = it;
	txBoolean async = (self->flags & mxAsyncFlag) ? 1 : 0;
	txCoder* coder = param;
	txTargetCode* target = fxCoderCreateTarget(coder);
	if (async) {
		fxNodeDispatchCode(self->expression, param);
	}
	else {
		fxCoderAddByte(param, 1, XS_CODE_OBJECT);
		fxCoderAddByte(param, 1, XS_CODE_DUB);
		fxNodeDispatchCode(self->expression, param);
		fxCoderAddSymbol(param, -2, XS_CODE_NEW_PROPERTY, coder->parser->valueSymbol);
		fxCoderAddInteger(param, 0, XS_CODE_INTEGER_1, 0);
		fxCoderAddByte(param, 1, XS_CODE_DUB);
		fxCoderAddByte(param, 1, XS_CODE_FALSE);
		fxCoderAddSymbol(param, -2, XS_CODE_NEW_PROPERTY, coder->parser->doneSymbol);
		fxCoderAddInteger(param, 0, XS_CODE_INTEGER_1, 0);
	}
	fxCoderAddByte(coder, 0, XS_CODE_YIELD);
	fxCoderAddBranch(coder, 1, XS_CODE_BRANCH_STATUS_1, target);
	if (async) {
		fxCoderAddByte(param, 0, XS_CODE_AWAIT);
		fxCoderAddByte(coder, 0, XS_CODE_THROW_STATUS);
	}
	fxCoderAddByte(param, -1, XS_CODE_SET_RESULT);
	fxCoderAdjustEnvironment(coder, coder->returnTarget);
	fxCoderAdjustScope(coder, coder->returnTarget);
	fxCoderAddBranch(coder, 0, XS_CODE_BRANCH_1, coder->returnTarget);
	fxCoderAdd(coder, 0, target);
}

