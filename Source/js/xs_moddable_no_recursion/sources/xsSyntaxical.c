/*
 * Copyright (c) 2016-2025  Moddable Tech, Inc.
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
#include <stdio.h>
extern void fxAbort(txMachine* the, int status);

static txBoolean fxIsKeyword(txParser* parser, txSymbol* keyword);
static txBoolean fxIsToken(txParser* parser, txToken theToken);
static void fxMatchToken(txParser* parser, txToken theToken);
static txNode* fxPopNode(txParser* parser);
static void fxPushNode(txParser* parser, txNode* node);
static void fxPushBigIntNode(txParser* parser, txBigInt* value, txInteger line);
static void fxPushIndexNode(txParser* parser, txIndex value, txInteger line);
static void fxPushIntegerNode(txParser* parser, txInteger value, txInteger line);
static void fxPushNodeStruct(txParser* parser, txInteger count, txToken token, txInteger line);
static void fxPushNodeList(txParser* parser, txInteger count);
static void fxPushNULL(txParser* parser);
static void fxPushNumberNode(txParser* parser, txNumber value, txInteger line);
static void fxPushRawNode(txParser* parser, txInteger length, txString value, txInteger line);
static void fxPushStringNode(txParser* parser, txInteger length, txString value, txInteger line);
static void fxPushSymbol(txParser* parser, txSymbol* symbol);
static void fxSwapNodes(txParser* parser);

static void fxExportDeclaration(txParser* parser);
static void fxExportBinding(txParser* parser, txNode* node);
static void fxImportDeclaration(txParser* parser);
static void fxWithAttributes(txParser* parser);
static void fxSpecifiers(txParser* parser);
static void fxParserReportWarning(txParser* parser, txInteger line, txString theFormat, ...);

static void fxBody(txParser* parser);
static void fxStatements(txParser* parser);
static void fxBlock(txParser* parser);
static void fxStatement(txParser* parser, txInteger blockIt);
static void fxSemicolon(txParser* parser);

static void fxBreakStatement(txParser* parser);
static void fxContinueStatement(txParser* parser);
static void fxDebuggerStatement(txParser* parser);
static void fxDoStatement(txParser* parser);
static void fxForStatement(txParser* parser);
static void fxIfStatement(txParser* parser);
static void fxReturnStatement(txParser* parser);
static void fxSwitchStatement(txParser* parser);
static void fxThrowStatement(txParser* parser);
static void fxTryStatement(txParser* parser);
static void fxWhileStatement(txParser* parser);
static void fxWithStatement(txParser* parser);

static void fxCommaExpression(txParser* parser);
static void fxAssignmentExpression(txParser* parser);
static void fxConditionalExpression(txParser* parser);
static void fxOrExpression(txParser* parser);
static void fxAndExpression(txParser* parser);
static void fxCoalesceExpression(txParser* parser);
static void fxBitOrExpression(txParser* parser);
static void fxBitXorExpression(txParser* parser);
static void fxBitAndExpression(txParser* parser);
static void fxEqualExpression(txParser* parser);
static void fxRelationalExpression(txParser* parser);
static void fxShiftExpression(txParser* parser);
static void fxAdditiveExpression(txParser* parser);
static void fxMultiplicativeExpression(txParser* parser);
static void fxExponentiationExpression(txParser* parser);
static void fxUnaryExpression(txParser* parser);
static void fxPrefixExpression(txParser* parser);
static void fxPostfixExpression(txParser* parser);
static void fxCallExpression(txParser* parser);

static void fxLiteralExpression(txParser* parser, txUnsigned flag);
static void fxArrayExpression(txParser* parser);
static void fxArrowExpression(txParser* parser, txUnsigned flag);
void fxClassExpression(txParser* parser, txInteger theLine, txSymbol** theSymbol);
static void fxFunctionExpression(txParser* parser, txInteger theLine, txSymbol** theSymbol, txUnsigned flag);
static void fxGeneratorExpression(txParser* parser, txInteger theLine, txSymbol** theSymbol, txUnsigned flag);
static void fxGroupExpression(txParser* parser, txUnsigned flag);
static void fxObjectExpression(txParser* parser);
static void fxNewExpression(txParser* parser);
static void fxTemplateExpression(txParser* parser);
static void fxYieldExpression(txParser* parser);

static void fxParameters(txParser* parser);
static void fxPropertyName(txParser* parser, txSymbol** theSymbol, txToken* theToken0, txToken* theToken1, txToken* theToken2, txUnsigned* flag);

static void fxBinding(txParser* parser, txToken theToken, txUnsigned flags);
txNode* fxBindingFromExpression(txParser* parser, txNode* theNode, txToken theToken);
static void fxArrayBinding(txParser* parser, txToken theToken);
static txNode* fxArrayBindingFromExpression(txParser* parser, txNode* theNode, txToken theToken);
static txUnsigned fxObjectBinding(txParser* parser, txToken theToken);
static txNode* fxObjectBindingFromExpression(txParser* parser, txNode* theNode, txToken theToken);
static void fxParametersBinding(txParser* parser);
static txNode* fxParametersBindingFromExpressions(txParser* parser, txNode* theNode);
static void fxRestBinding(txParser* parser, txToken theToken, txUnsigned flag);
static txNode* fxRestBindingFromExpression(txParser* parser, txNode* theNode, txToken theToken, txUnsigned flag);

static void fxCheckArrowFunction(txParser* parser, txInteger count);
static void fxCheckNativeConstructor(txParser* parser);
static void fxCheckNativeFunction(txParser* parser);
static txBoolean fxCheckReference(txParser* parser, txToken theToken);
static void fxCheckStrictBinding(txParser* parser, txNode* node);
static void fxCheckStrictFunction(txParser* parser, txFunctionNode* function);
static void fxCheckStrictSymbol(txParser* parser, txSymbol* symbol);
static void fxCheckUniqueProperty(txParser* parser, txNode* base, txNode* current);
static void fxCheckUniquePropertyAux(txParser* parser, txNode* baseNode, txNode* currentNode);

static void fxJSONObject(txParser* parser);
static void fxJSONArray(txParser* parser);

static void fxJSXAttributeName(txParser* parser);
static void fxJSXAttributeValue(txParser* parser);
static void fxJSXElement(txParser* parser);
static void fxJSXElementName(txParser* parser);
static txBoolean fxJSXMatch(txParser* parser, txNode* opening, txNode* closing);
static txSymbol* fxJSXNamespace(txParser* parser, txSymbol* namespace, txSymbol* name);
static txSymbol* fxJSXName(txParser* parser, txSymbol* before, txSymbol* after);


/* xs_no_recursion: parser trampoline core (see fxParserPump) */

/* R19 diagnostics: when the pump guard below fires, the bridge's fxAbort
   logs these so the device log shows the corrupt kind and which memory the
   frame came from (live parser chunk = overflow; anything else = stale
   frame / use-after-free of a disposed chunk). */
long gXSNRFaultKind = -1;
void* gXSNRFaultFrame = NULL;
void* gXSNRFaultChunkFirst = NULL;
void* gXSNRFaultChunkCur = NULL;
/* Stalled symbol name for the R19 symbol-chain guards (intern walk in
   xsScript.c, serialize walk in xsCode.c). */
char gXSNRFaultSym[32] = {0};

enum {
	K_PROGRAM,
	K_MODULE,
	K_STATEMENTS,
	K_STATEMENT,
	K_BLOCK,
	K_DO,
	K_FOR,
	K_IF,
	K_SWITCH,
	K_TRY,
	K_VARIABLE_STATEMENT,
	K_WHILE,
	K_WITH,
	K_COMMA,
	K_ASSIGN,
	K_CONDITIONAL,
	K_COALESCE,
	K_OR,
	K_AND,
	K_BIT_OR,
	K_BIT_XOR,
	K_BIT_AND,
	K_EQUAL,
	K_RELATIONAL,
	K_SHIFT,
	K_ADDITIVE,
	K_MULTIPLICATIVE,
	K_EXPONENTIATION,
	K_UNARY,
	K_PREFIX,
	K_POSTFIX,
	K_CALL_CHAIN,
	K_LITERAL,
	K_ARRAY_EXPR,
	K_ARROW,
	K_CLASS_EXPR,
	K_FUNCTION_EXPR,
	K_GENERATOR_EXPR,
	K_GROUP_EXPR,
	K_NEW_EXPR,
	K_OBJECT_EXPR,
	K_TEMPLATE_EXPR,
	K_YIELD_EXPR,
	K_PARAMETERS,
	K_BODY,
	K_PROPERTY_NAME,
	K_BINDING,
	K_ARRAY_BINDING,
	K_OBJECT_BINDING,
	K_PARAMETERS_BINDING,
	K_REST_BINDING,
	K_EXPORT_DECLARATION,
	K_IMPORT_DECLARATION,
	K_JSON_VALUE,
	K_JSON_OBJECT,
	K_JSON_ARRAY,
	K_JSX_ELEMENT,
	K_JSX_ATTRIBUTE_VALUE,
	K_CHECK_STRICT_BINDING,
	K_PARAMS_BINDING_FROM,
	K_KIND_COUNT
};

typedef struct sxParserFrame txParserFrame;

struct sxParserFrame {
	struct sxParserFrame* next;
	txInteger kind;
	txInteger pc;
	txInteger line;
	/* integer slots */
	txInteger i0, i1, i2, i3, i4, i5;
	txInteger blockIt;
	/* unsigned / flag slots */
	txUnsigned u0, u1, u2;
	/* token slots */
	txToken t0, t1, t2;
	/* node slots */
	txNode* n0;
	txNode* n1;
	txNode* n2;
	txNode* n3;
	/* symbol slots */
	txSymbol* s0;
	txSymbol* s1;
	/* boolean slots */
	int b0, b1, b2, b3;
};

#define P_FRAME 		(parser->curFrame)
#define P_RESUME(_pc) 	do { f->pc = (_pc); return; } while (0)

/* xs_no_recursion: step routine prototypes (used by fxParserRunStep) */
static void fxProgramStep(txParser* parser); /* R22: K_PROGRAM frame body */
static void fxStatementsStep(txParser* parser);
static void fxStatementStep(txParser* parser);
static void fxBlockStep(txParser* parser);
static void fxDoStatementStep(txParser* parser);
static void fxForStatementStep(txParser* parser);
static void fxIfStatementStep(txParser* parser);
static void fxSwitchStatementStep(txParser* parser);
static void fxTryStatementStep(txParser* parser);
static void fxVariableStatementStep(txParser* parser);
static void fxWhileStatementStep(txParser* parser);
static void fxWithStatementStep(txParser* parser);
static void fxCommaExpressionStep(txParser* parser);
static void fxAssignmentExpressionStep(txParser* parser);
static void fxConditionalExpressionStep(txParser* parser);
static void fxCoalesceExpressionStep(txParser* parser);
static void fxOrExpressionStep(txParser* parser);
static void fxAndExpressionStep(txParser* parser);
static void fxBitOrExpressionStep(txParser* parser);
static void fxBitXorExpressionStep(txParser* parser);
static void fxBitAndExpressionStep(txParser* parser);
static void fxEqualExpressionStep(txParser* parser);
static void fxRelationalExpressionStep(txParser* parser);
static void fxShiftExpressionStep(txParser* parser);
static void fxAdditiveExpressionStep(txParser* parser);
static void fxMultiplicativeExpressionStep(txParser* parser);
static void fxExponentiationExpressionStep(txParser* parser);
static void fxUnaryExpressionStep(txParser* parser);
static void fxPrefixExpressionStep(txParser* parser);
static void fxPostfixExpressionStep(txParser* parser);
static void fxCallExpressionStep(txParser* parser);
static void fxLiteralExpressionStep(txParser* parser);
static void fxArrayExpressionStep(txParser* parser);
static void fxArrowExpressionStep(txParser* parser);
static void fxClassExpressionStep(txParser* parser);
static void fxFunctionExpressionStep(txParser* parser);
static void fxGeneratorExpressionStep(txParser* parser);
static void fxGroupExpressionStep(txParser* parser);
static void fxNewExpressionStep(txParser* parser);
static void fxObjectExpressionStep(txParser* parser);
static void fxTemplateExpressionStep(txParser* parser);
static void fxYieldExpressionStep(txParser* parser);
static void fxParametersStep(txParser* parser);
static void fxBodyStep(txParser* parser);
static void fxCheckStrictBindingStep(txParser* parser);
static void fxParametersBindingFromStep(txParser* parser);
void fxParserCallClassFlag(txParser* parser, txInteger kind, txInteger line, txUnsigned u0, txSymbol** theSymbol);
static void fxPropertyNameStep(txParser* parser);
static void fxBindingStep(txParser* parser);
static void fxArrayBindingStep(txParser* parser);
static void fxObjectBindingStep(txParser* parser);
static void fxParametersBindingStep(txParser* parser);
static void fxRestBindingStep(txParser* parser);
static void fxJSONValueStep(txParser* parser);
static void fxJSONObjectStep(txParser* parser);
static void fxJSONArrayStep(txParser* parser);
static void fxJSXElementStep(txParser* parser);
static void fxJSXAttributeValueStep(txParser* parser);




/* xs_no_recursion: resumable step routines, one per recursive rule */
#define XS_TOKEN_BEGIN_STATEMENT 1
#define XS_TOKEN_BEGIN_EXPRESSION 2
#define XS_TOKEN_ASSIGN_EXPRESSION 4
#define XS_TOKEN_EQUAL_EXPRESSION 8
#define XS_TOKEN_RELATIONAL_EXPRESSION 16
#define XS_TOKEN_SHIFT_EXPRESSION 32
#define XS_TOKEN_ADDITIVE_EXPRESSION 64
#define XS_TOKEN_MULTIPLICATIVE_EXPRESSION 128
#define XS_TOKEN_EXPONENTIATION_EXPRESSION 256
#define XS_TOKEN_PREFIX_EXPRESSION 512
#define XS_TOKEN_POSTFIX_EXPRESSION 1024
#define XS_TOKEN_END_STATEMENT 2048
#define XS_TOKEN_REFERENCE_EXPRESSION 4096
#define XS_TOKEN_BEGIN_BINDING 16384
#define XS_TOKEN_IDENTIFIER_NAME 32768
#define XS_TOKEN_UNARY_EXPRESSION 65536
#define XS_TOKEN_CALL_EXPRESSION 131072

static txTokenFlag gxTokenFlags[XS_TOKEN_COUNT] = {
	/* XS_NO_TOKEN */ 0,
	/* XS_TOKEN_ACCESS */ 0,
	/* XS_TOKEN_ADD */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_ADDITIVE_EXPRESSION | XS_TOKEN_UNARY_EXPRESSION,
	/* XS_TOKEN_ADD_ASSIGN */ XS_TOKEN_ASSIGN_EXPRESSION,
	/* XS_TOKEN_AND */ 0,
	/* XS_TOKEN_AND_ASSIGN */ XS_TOKEN_ASSIGN_EXPRESSION,
	/* XS_TOKEN_ARG */ 0,
	/* XS_TOKEN_ARGUMENTS */ 0,
	/* XS_TOKEN_ARGUMENTS_SLOPPY */ 0,
	/* XS_TOKEN_ARGUMENTS_STRICT */ 0,
	/* XS_TOKEN_ARRAY */ 0,
	/* XS_TOKEN_ARRAY_BINDING */ 0,
	/* XS_TOKEN_ARROW */ 0,
	/* XS_TOKEN_ASSIGN */ XS_TOKEN_ASSIGN_EXPRESSION,
	/* XS_TOKEN_AWAIT */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_UNARY_EXPRESSION | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_BIGINT */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION,
	/* XS_TOKEN_BINDING */ 0,
	/* XS_TOKEN_BIT_AND */ 0,
	/* XS_TOKEN_BIT_AND_ASSIGN */ XS_TOKEN_ASSIGN_EXPRESSION,
	/* XS_TOKEN_BIT_NOT */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_UNARY_EXPRESSION,
	/* XS_TOKEN_BIT_OR */ 0,
	/* XS_TOKEN_BIT_OR_ASSIGN */ XS_TOKEN_ASSIGN_EXPRESSION,
	/* XS_TOKEN_BIT_XOR */ 0,
	/* XS_TOKEN_BIT_XOR_ASSIGN */ XS_TOKEN_ASSIGN_EXPRESSION,
	/* XS_TOKEN_BLOCK */ 0,
	/* XS_TOKEN_BODY */ 0,
	/* XS_TOKEN_BREAK */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_CALL */ 0,
	/* XS_TOKEN_CASE */ XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_CATCH */ XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_CHAIN */ XS_TOKEN_CALL_EXPRESSION,
	/* XS_TOKEN_CLASS */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_COALESCE */ 0,
	/* XS_TOKEN_COALESCE_ASSIGN */ XS_TOKEN_ASSIGN_EXPRESSION,
	/* XS_TOKEN_COLON */ 0,
	/* XS_TOKEN_COMMA */ XS_TOKEN_END_STATEMENT,
	/* XS_TOKEN_CONST */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_CONTINUE */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_CURRENT */ 0,
	/* XS_TOKEN_DEBUGGER */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_DECREMENT */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_PREFIX_EXPRESSION | XS_TOKEN_POSTFIX_EXPRESSION,
	/* XS_TOKEN_DEFAULT */ XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_DEFINE */ 0,
	/* XS_TOKEN_DELEGATE */ 0,
	/* XS_TOKEN_DELETE */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_UNARY_EXPRESSION | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_DIVIDE */ XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_MULTIPLICATIVE_EXPRESSION,
	/* XS_TOKEN_DIVIDE_ASSIGN */ XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_ASSIGN_EXPRESSION,
	/* XS_TOKEN_DO */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_DOT */ XS_TOKEN_CALL_EXPRESSION,
	/* XS_TOKEN_ELISION */ 0,
	/* XS_TOKEN_ELSE */ XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_ENUM */ XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_EOF */ XS_TOKEN_END_STATEMENT,
	/* XS_TOKEN_EQUAL */ XS_TOKEN_EQUAL_EXPRESSION,
	/* XS_TOKEN_EVAL */ 0,
	/* XS_TOKEN_EXPONENTIATION */ XS_TOKEN_EXPONENTIATION_EXPRESSION,
	/* XS_TOKEN_EXPONENTIATION_ASSIGN */ XS_TOKEN_ASSIGN_EXPRESSION,
	/* XS_TOKEN_EXPORT */ XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_EXPRESSIONS */ 0,
	/* XS_TOKEN_EXTENDS */ XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_FALSE */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_FIELD */ 0,
	/* XS_TOKEN_FINALLY */ XS_TOKEN_IDENTIFIER_NAME | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_FOR */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_FOR_AWAIT_OF */ 0,
	/* XS_TOKEN_FOR_IN */ 0,
	/* XS_TOKEN_FOR_OF */ 0,
	/* XS_TOKEN_FUNCTION */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_GENERATOR */ 0,
	/* XS_TOKEN_GETTER */ 0,
	/* XS_TOKEN_HOST */ XS_TOKEN_BEGIN_EXPRESSION,
	/* XS_TOKEN_IDENTIFIER */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_BEGIN_BINDING | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_IF */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_IMPLEMENTS */ XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_IMPORT */ XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_IMPORT_CALL */ 0,
	/* XS_TOKEN_IMPORT_META */ 0,
	/* XS_TOKEN_IN */ XS_TOKEN_RELATIONAL_EXPRESSION | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_INCLUDE */ 0,
	/* XS_TOKEN_INCREMENT */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_PREFIX_EXPRESSION | XS_TOKEN_POSTFIX_EXPRESSION,
	/* XS_TOKEN_INSTANCEOF */ XS_TOKEN_RELATIONAL_EXPRESSION | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_INTEGER */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION,
	/* XS_TOKEN_INTERFACE */ XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_ITEMS */ 0,
	/* XS_TOKEN_LABEL */ 0,
	/* XS_TOKEN_LEFT_BRACE */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_BEGIN_BINDING,
	/* XS_TOKEN_LEFT_BRACKET */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_BEGIN_BINDING | XS_TOKEN_CALL_EXPRESSION,
	/* XS_TOKEN_LEFT_PARENTHESIS */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_CALL_EXPRESSION,
	/* XS_TOKEN_LEFT_SHIFT */ XS_TOKEN_SHIFT_EXPRESSION,
	/* XS_TOKEN_LEFT_SHIFT_ASSIGN */ XS_TOKEN_ASSIGN_EXPRESSION,
	/* XS_TOKEN_LESS */ XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_RELATIONAL_EXPRESSION,
	/* XS_TOKEN_LESS_EQUAL */ XS_TOKEN_RELATIONAL_EXPRESSION,
	/* XS_TOKEN_LET */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_MEMBER */ 0,
	/* XS_TOKEN_MEMBER_AT */ 0,
	/* XS_TOKEN_MINUS */ 0,
	/* XS_TOKEN_MODULE */ 0,
	/* XS_TOKEN_MODULO */ XS_TOKEN_MULTIPLICATIVE_EXPRESSION,
	/* XS_TOKEN_MODULO_ASSIGN */ XS_TOKEN_ASSIGN_EXPRESSION,
	/* XS_TOKEN_MORE */ XS_TOKEN_RELATIONAL_EXPRESSION,
	/* XS_TOKEN_MORE_EQUAL */ XS_TOKEN_RELATIONAL_EXPRESSION,
	/* XS_TOKEN_MULTIPLY */ XS_TOKEN_MULTIPLICATIVE_EXPRESSION,
	/* XS_TOKEN_MULTIPLY_ASSIGN */ XS_TOKEN_ASSIGN_EXPRESSION,
	/* XS_TOKEN_NEW */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_IDENTIFIER_NAME, 
	/* XS_TOKEN_NOT */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_UNARY_EXPRESSION,
	/* XS_TOKEN_NOT_EQUAL */ XS_TOKEN_EQUAL_EXPRESSION,
	/* XS_TOKEN_NULL */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_IDENTIFIER_NAME, 
	/* XS_TOKEN_NUMBER */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION,
	/* XS_TOKEN_OBJECT */ 0,
	/* XS_TOKEN_OBJECT_BINDING */ 0,
	/* XS_TOKEN_OPTION */ 0,
	/* XS_TOKEN_OR */ 0,
	/* XS_TOKEN_OR_ASSIGN */ XS_TOKEN_ASSIGN_EXPRESSION,
	/* XS_TOKEN_PACKAGE */ XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_PARAMS */ 0,
	/* XS_TOKEN_PARAMS_BINDING */ 0,
	/* XS_TOKEN_PLUS */ 0,
	/* XS_TOKEN_PRIVATE */ XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_PRIVATE_IDENTIFIER */ XS_TOKEN_BEGIN_EXPRESSION,
	/* XS_TOKEN_PRIVATE_MEMBER */ 0,
	/* XS_TOKEN_PRIVATE_PROPERTY */ 0,
	/* XS_TOKEN_PROGRAM */ 0,
	/* XS_TOKEN_PROPERTY */ 0,
	/* XS_TOKEN_PROPERTY_AT */ 0,
	/* XS_TOKEN_PROPERTY_BINDING */ 0,
	/* XS_TOKEN_PROPERTY_BINDING_AT */ 0,
	/* XS_TOKEN_PROTECTED */ XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_PUBLIC */ XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_QUESTION_MARK */ 0,
	/* XS_TOKEN_REGEXP */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION,
	/* XS_TOKEN_REST_BINDING */ 0,
	/* XS_TOKEN_RETURN */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_RIGHT_BRACE */ XS_TOKEN_END_STATEMENT,
	/* XS_TOKEN_RIGHT_BRACKET */ 0,
	/* XS_TOKEN_RIGHT_PARENTHESIS */ 0,
	/* XS_TOKEN_SEMICOLON */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_END_STATEMENT,
	/* XS_TOKEN_SETTER */ 0,
	/* XS_TOKEN_SHORT */ 0,
	/* XS_TOKEN_SIGNED_RIGHT_SHIFT */ XS_TOKEN_SHIFT_EXPRESSION,
	/* XS_TOKEN_SIGNED_RIGHT_SHIFT_ASSIGN */ XS_TOKEN_ASSIGN_EXPRESSION,
	/* XS_TOKEN_SKIP_BINDING */ 0,
	/* XS_TOKEN_SPECIFIER */ 0,
	/* XS_TOKEN_SPREAD */ XS_TOKEN_BEGIN_BINDING,
	/* XS_TOKEN_STATEMENT */ 0,
	/* XS_TOKEN_STATEMENTS */ 0,
	/* XS_TOKEN_STATIC */ XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_STRICT_EQUAL */ XS_TOKEN_EQUAL_EXPRESSION,
	/* XS_TOKEN_STRICT_NOT_EQUAL */ XS_TOKEN_EQUAL_EXPRESSION,
	/* XS_TOKEN_STRING */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION,
	/* XS_TOKEN_SUBTRACT */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_ADDITIVE_EXPRESSION | XS_TOKEN_UNARY_EXPRESSION,
	/* XS_TOKEN_SUBTRACT_ASSIGN */ XS_TOKEN_ASSIGN_EXPRESSION,
	/* XS_TOKEN_SUPER */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_SWITCH */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_TARGET */ 0,
	/* XS_TOKEN_TEMPLATE */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_CALL_EXPRESSION,
	/* XS_TOKEN_TEMPLATE_HEAD */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_CALL_EXPRESSION,
	/* XS_TOKEN_TEMPLATE_MIDDLE */ 0,
	/* XS_TOKEN_TEMPLATE_TAIL */ 0,
	/* XS_TOKEN_THIS */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_THROW */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_TRUE */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_TRY */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_TYPEOF */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_UNARY_EXPRESSION | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_UNDEFINED */ 0,
	/* XS_TOKEN_UNSIGNED_RIGHT_SHIFT */ XS_TOKEN_SHIFT_EXPRESSION,
	/* XS_TOKEN_UNSIGNED_RIGHT_SHIFT_ASSIGN */ XS_TOKEN_ASSIGN_EXPRESSION,
	/* XS_TOKEN_USING */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_VAR */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_VOID */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_UNARY_EXPRESSION | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_WHILE */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_WITH */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_IDENTIFIER_NAME,
	/* XS_TOKEN_YIELD */ XS_TOKEN_BEGIN_STATEMENT | XS_TOKEN_BEGIN_EXPRESSION | XS_TOKEN_IDENTIFIER_NAME
};

static txString const gxTokenNames[XS_TOKEN_COUNT] ICACHE_FLASH_ATTR = {
	/* XS_NO_TOKEN */ "",
	/* XS_TOKEN_ACCESS */ "access",
	/* XS_TOKEN_ADD */ "+",
	/* XS_TOKEN_ADD_ASSIGN */ "+=",
	/* XS_TOKEN_AND */ "&&",
	/* XS_TOKEN_AND_ASSIGN */ "&&=",
	/* XS_TOKEN_ARG */ "arg",
	/* XS_TOKEN_ARGUMENTS */ "arguments",
	/* XS_TOKEN_ARGUMENTS_SLOPPY */ "arguments_sloppy",
	/* XS_TOKEN_ARGUMENTS_STRICT */ "arguments_strict",
	/* XS_TOKEN_ARRAY */ "array",
	/* XS_TOKEN_ARRAY_BINDING */ "array_binding",
	/* XS_TOKEN_ARROW */ "=>",
	/* XS_TOKEN_ASSIGN */ "=",
	/* XS_TOKEN_AWAIT */ "await",
	/* XS_TOKEN_BIGINT */ "bigint",
	/* XS_TOKEN_BINDING */ "binding",
	/* XS_TOKEN_BIT_AND */ "&",
	/* XS_TOKEN_BIT_AND_ASSIGN */ "&=",
	/* XS_TOKEN_BIT_NOT */ "~",
	/* XS_TOKEN_BIT_OR */ "|",
	/* XS_TOKEN_BIT_OR_ASSIGN */ "|=",
	/* XS_TOKEN_BIT_XOR */ "^",
	/* XS_TOKEN_BIT_XOR_ASSIGN */ "^=",
	/* XS_TOKEN_BLOCK */ "block",
	/* XS_TOKEN_BODY */ "body",
	/* XS_TOKEN_BREAK */ "break",
	/* XS_TOKEN_CALL */ "call",
	/* XS_TOKEN_CASE */ "case",
	/* XS_TOKEN_CATCH */ "catch",
	/* XS_TOKEN_CHAIN */ "?.",
	/* XS_TOKEN_CLASS */ "class",
	/* XS_TOKEN_COALESCE */ "??",
	/* XS_TOKEN_COALESCE_ASSIGN */ "\?\?=",
	/* XS_TOKEN_COLON */ ":",
	/* XS_TOKEN_COMMA */ ",",
	/* XS_TOKEN_CONST */ "const",
	/* XS_TOKEN_CONTINUE */ "continue",
	/* XS_TOKEN_CURRENT */ "current",
	/* XS_TOKEN_DEBUGGER */ "debugger",
	/* XS_TOKEN_DECREMENT */ "--",
	/* XS_TOKEN_DEFAULT */ "default",
	/* XS_TOKEN_DEFINE */ "define",
	/* XS_TOKEN_DELEGATE */ "delegate",
	/* XS_TOKEN_DELETE */ "delete",
	/* XS_TOKEN_DIVIDE */ "/",
	/* XS_TOKEN_DIVIDE_ASSIGN */ "/=",
	/* XS_TOKEN_DO */ "do",
	/* XS_TOKEN_DOT */ ".",
	/* XS_TOKEN_ELISION */ "elision",
	/* XS_TOKEN_ELSE */ "else",
	/* XS_TOKEN_ENUM */ "enum",
	/* XS_TOKEN_EOF */ "",
	/* XS_TOKEN_EQUAL */ "==",
	/* XS_TOKEN_EVAL */ "eval",
	/* XS_TOKEN_EXPONENTIATION */ "**",
	/* XS_TOKEN_EXPONENTIATION_ASSIGN */ "**=",
	/* XS_TOKEN_EXPORT */ "export",
	/* XS_TOKEN_EXPRESSIONS */ "expressions",
	/* XS_TOKEN_EXTENDS */ "extends",
	/* XS_TOKEN_FALSE */ "false",
	/* XS_TOKEN_FIELD */ "field",
	/* XS_TOKEN_FINALLY */ "finally",
	/* XS_TOKEN_FOR */ "for",
	/* XS_TOKEN_FOR_AWAIT_OF */ "for_await_of",
	/* XS_TOKEN_FOR_IN */ "for_in",
	/* XS_TOKEN_FOR_OF */ "for_of",
	/* XS_TOKEN_FUNCTION */ "function",
	/* XS_TOKEN_GENERATOR */ "generator",
	/* XS_TOKEN_GETTER */ "getter",
	/* XS_TOKEN_HOST */ "host", 
	/* XS_TOKEN_IDENTIFIER */ "identifier",
	/* XS_TOKEN_IF */ "if",
	/* XS_TOKEN_IMPLEMENTS */ "implements",
	/* XS_TOKEN_IMPORT */ "import",
	/* XS_TOKEN_IMPORT_CALL */ "import",
	/* XS_TOKEN_IMPORT_META */ "import.meta",
	/* XS_TOKEN_IN */ "in",
	/* XS_TOKEN_INCLUDE */ "include",
	/* XS_TOKEN_INCREMENT */ "++",
	/* XS_TOKEN_INSTANCEOF */ "instanceof",
	/* XS_TOKEN_INTEGER */ "integer",
	/* XS_TOKEN_INTERFACE */ "interface",
	/* XS_TOKEN_ITEMS */ "items",
	/* XS_TOKEN_LABEL */ "label",
	/* XS_TOKEN_LEFT_BRACE */ "{",
	/* XS_TOKEN_LEFT_BRACKET */ "[",
	/* XS_TOKEN_LEFT_PARENTHESIS */ "(",
	/* XS_TOKEN_LEFT_SHIFT */ "<<",
	/* XS_TOKEN_LEFT_SHIFT_ASSIGN */ "<<=",
	/* XS_TOKEN_LESS */ "<",
	/* XS_TOKEN_LESS_EQUAL */ "<=",
	/* XS_TOKEN_LET */ "let",
	/* XS_TOKEN_MEMBER */ "member",
	/* XS_TOKEN_MEMBER_AT */ "member_at",
	/* XS_TOKEN_MINUS */ "minus",
	/* XS_TOKEN_MODULE */ "module",
	/* XS_TOKEN_MODULO */ "%",
	/* XS_TOKEN_MODULO_ASSIGN */ "%=",
	/* XS_TOKEN_MORE */ ">",
	/* XS_TOKEN_MORE_EQUAL */ ">=",
	/* XS_TOKEN_MULTIPLY */ "*",
	/* XS_TOKEN_MULTIPLY_ASSIGN */ "*=",
	/* XS_TOKEN_NEW */ "new", 
	/* XS_TOKEN_NOT */ "!",
	/* XS_TOKEN_NOT_EQUAL */ "!=",
	/* XS_TOKEN_NULL */ "null", 
	/* XS_TOKEN_NUMBER */ "number",
	/* XS_TOKEN_OBJECT */ "object",
	/* XS_TOKEN_OBJECT_BINDING */ "object_binding",
	/* XS_TOKEN_OPTION */ "?.",
	/* XS_TOKEN_OR */ "||",
	/* XS_TOKEN_OR_ASSIGN */ "||=",
	/* XS_TOKEN_PACKAGE */ "package",
	/* XS_TOKEN_PARAMS */ "params",
	/* XS_TOKEN_PARAMS_BINDING */ "params_binding",
	/* XS_TOKEN_PLUS */ "plus",
	/* XS_TOKEN_PRIVATE */ "private",
	/* XS_TOKEN_PRIVATE_IDENTIFIER */ "private_identifier",
	/* XS_TOKEN_PRIVATE_MEMBER */ "private_member",
	/* XS_TOKEN_PRIVATE_PROPERTY */ "private_property",
	/* XS_TOKEN_PROGRAM */ "program",
	/* XS_TOKEN_PROPERTY */ "property",
	/* XS_TOKEN_PROPERTY_AT */ "property_at",
	/* XS_TOKEN_PROPERTY_BINDING */ "property_binding",
	/* XS_TOKEN_PROPERTY_BINDING_AT */ "property_binding_at",
	/* XS_TOKEN_PROTECTED */ "protected",
	/* XS_TOKEN_PUBLIC */ "public",
	/* XS_TOKEN_QUESTION_MARK */ "?",
	/* XS_TOKEN_REGEXP */ "regexp",
	/* XS_TOKEN_REST_BINDING */ "rest_binding",
	/* XS_TOKEN_RETURN */ "return",
	/* XS_TOKEN_RIGHT_BRACE */ "}",
	/* XS_TOKEN_RIGHT_BRACKET */ "]",
	/* XS_TOKEN_RIGHT_PARENTHESIS */ ")",
	/* XS_TOKEN_SEMICOLON */ ";",
	/* XS_TOKEN_SETTER */ "setter",
	/* XS_TOKEN_SHORT */ "short",
	/* XS_TOKEN_SIGNED_RIGHT_SHIFT */ ">>",
	/* XS_TOKEN_SIGNED_RIGHT_SHIFT_ASSIGN */ ">>=",
	/* XS_TOKEN_SKIP_BINDING */ "skip_binding",
	/* XS_TOKEN_SPECIFIER */ "specifier",
	/* XS_TOKEN_SPREAD */ "...",
	/* XS_TOKEN_STATEMENT */ "statement",
	/* XS_TOKEN_STATEMENTS */ "statements",
	/* XS_TOKEN_STATIC */ "static",
	/* XS_TOKEN_STRICT_EQUAL */ "===",
	/* XS_TOKEN_STRICT_NOT_EQUAL */ "!==",
	/* XS_TOKEN_STRING */ "string",
	/* XS_TOKEN_SUBTRACT */ "-",
	/* XS_TOKEN_SUBTRACT_ASSIGN */ "-=",
	/* XS_TOKEN_SUPER */ "super",
	/* XS_TOKEN_SWITCH */ "switch",
	/* XS_TOKEN_TARGET */ "target",
	/* XS_TOKEN_TEMPLATE */ "template",
	/* XS_TOKEN_TEMPLATE_HEAD */ "template_head",
	/* XS_TOKEN_TEMPLATE_MIDDLE */ "template_middle",
	/* XS_TOKEN_TEMPLATE_TAIL */ "template_tail",
	/* XS_TOKEN_THIS */ "this",
	/* XS_TOKEN_THROW */ "throw",
	/* XS_TOKEN_TRUE */ "true",
	/* XS_TOKEN_TRY */ "try",
	/* XS_TOKEN_TYPEOF */ "typeof",
	/* XS_TOKEN_UNDEFINED */ "undefined",
	/* XS_TOKEN_UNSIGNED_RIGHT_SHIFT */ ">>>",
	/* XS_TOKEN_UNSIGNED_RIGHT_SHIFT_ASSIGN */ ">>>=",
	/* XS_TOKEN_USIGN */ "using",
	/* XS_TOKEN_VAR */ "var",
	/* XS_TOKEN_VOID */ "void",
	/* XS_TOKEN_WHILE */ "while",
	/* XS_TOKEN_WITH */ "with",
	/* XS_TOKEN_YIELD */ "yield",
};

/* xs_no_recursion: trampoline call/return/pump implementation */
static void fxParserRunStep(txParser* parser, txInteger kind);

static txParserFrame* fxParserFramePush(txParser* parser, txInteger kind, txInteger line)
{
	txParserFrame* frame = parser->framePool;
	if (frame)
		parser->framePool = frame->next;
	else
		frame = (txParserFrame*)fxNewParserChunkClear(parser, sizeof(txParserFrame));
	frame->next = parser->curFrame;
	frame->kind = kind;
	frame->pc = 0;
	frame->line = line;
	frame->i0 = 0;
	frame->i1 = 0;
	frame->i2 = 0;
	frame->i3 = 0;
	frame->i4 = 0;
	frame->i5 = 0;
	frame->blockIt = 0;
	frame->u0 = 0;
	frame->u1 = 0;
	frame->u2 = 0;
	frame->t0 = XS_NO_TOKEN;
	frame->t1 = XS_NO_TOKEN;
	frame->t2 = XS_NO_TOKEN;
	frame->n0 = NULL;
	frame->n1 = NULL;
	frame->n2 = NULL;
	frame->n3 = NULL;
	frame->s0 = NULL;
	frame->s1 = NULL;
	frame->b0 = 0;
	frame->b1 = 0;
	frame->b2 = 0;
	frame->b3 = 0;
#ifdef XS_NR_PUMP_TRACE
	{ static unsigned long gFrameSerial = 0; frame->i5 = (txInteger)(++gFrameSerial); }
	c_fprintf(stderr, "PUSHFR id=%d f=%p next=%p kind=%d nc=%d\n", (int)frame->i5, (void*)frame, (void*)frame->next, (int)frame->kind, (int)parser->nodeCount);
#endif
	parser->curFrame = frame;
	return frame;
}

static void fxParserFramePop(txParser* parser)
{
	txParserFrame* frame = parser->curFrame;
#ifdef XS_NR_PUMP_TRACE
	c_fprintf(stderr, "POPFR id=%d f=%p -> next=%p nc=%d\n", (int)frame->i5, (void*)frame, (void*)frame->next, (int)parser->nodeCount);
#endif
	parser->curFrame = frame->next;
	frame->next = parser->framePool;
	parser->framePool = frame;
}

static void fxParserReturn(txParser* parser)
{
	/* publish the returning frame's out-slots (symbol name of a
	   function/class, property-name parts) for the resuming caller */
#ifdef XS_NR_PUMP_TRACE
	{ static unsigned long pn = 0; if (pn++ < 80) c_fprintf(stderr, "POP kind=%d pc=%d\n", (int)parser->curFrame->kind, (int)parser->curFrame->pc); }
#endif
	parser->outSymbol = parser->curFrame->s0;
	parser->outToken0 = parser->curFrame->t0;
	parser->outToken1 = parser->curFrame->t1;
	parser->outToken2 = parser->curFrame->t2;
	parser->outFlags = parser->curFrame->u0;
	fxParserFramePop(parser);
}

/* R19: every parser frame lives inside a live parser chunk (frames are
   only ever handed out by fxNewParserChunkClear). If the current frame is
   outside all live chunks, the frame is stale — a use-after-free of a
   disposed chunk that the SDK allocator recycled. Without this check such
   a frame dispatches a garbage kind through the -O2 switch jump table =
   wild jump with no diagnostic. One compare per chunk link per dispatch;
   chunk chains are 1–4 links in practice. */
static int fxNRFrameInLiveChunks(txParser* parser, void* frame)
{
	txParserChunk* block = parser->first;
	while (block) {
		txByte* start = (txByte*)(block + 1);
		txByte* end = (txByte*)block + kParserChunkSize;
		if (((txByte*)frame >= start) && ((txByte*)frame < end))
			return 1;
		block = block->next;
	}
	return 0;
}

static void fxParserRunStep(txParser* parser, txInteger kind)
{
	if ((kind < 0) || (kind >= K_KIND_COUNT) || (parser->curFrame == C_NULL) ||
		!fxNRFrameInLiveChunks(parser, parser->curFrame)) {
		gXSNRFaultKind = (long)kind;
		gXSNRFaultFrame = parser->curFrame;
		gXSNRFaultChunkFirst = parser->first;
		gXSNRFaultChunkCur = parser->chunk;
		fxAbort(parser->console, XS_PUMP_CORRUPTION_EXIT);
	}
#ifdef XS_NR_PUMP_TRACE
	{ static unsigned long rn = 0; if (rn++ < 200000) c_fprintf(stderr, "RUN id=%d kind=%d pc=%d f=%p base=%p pump=%d nc=%d\n", (int)parser->curFrame->i5, (int)kind, (int)parser->curFrame->pc, (void*)parser->curFrame, (void*)parser->pumpBase, (int)parser->pumpRunning, (int)parser->nodeCount); }
#endif
	switch (kind) {
		case K_BLOCK:				fxBlockStep(parser);			break;
		case K_DO:					fxDoStatementStep(parser);		break;
		case K_FOR:					fxForStatementStep(parser);		break;
		case K_IF:					fxIfStatementStep(parser);		break;
		case K_SWITCH:				fxSwitchStatementStep(parser);	break;
		case K_TRY:					fxTryStatementStep(parser);		break;
		case K_VARIABLE_STATEMENT:	fxVariableStatementStep(parser);	break;
		case K_WHILE:				fxWhileStatementStep(parser);	break;
		case K_WITH:				fxWithStatementStep(parser);	break;
		case K_COMMA:				fxCommaExpressionStep(parser);	break;
		case K_ASSIGN:				fxAssignmentExpressionStep(parser);	break;
		case K_CONDITIONAL:			fxConditionalExpressionStep(parser);	break;
		case K_COALESCE:			fxCoalesceExpressionStep(parser);	break;
		case K_OR:					fxOrExpressionStep(parser);		break;
		case K_AND:					fxAndExpressionStep(parser);		break;
		case K_BIT_OR:				fxBitOrExpressionStep(parser);	break;
		case K_BIT_XOR:				fxBitXorExpressionStep(parser);	break;
		case K_BIT_AND:				fxBitAndExpressionStep(parser);	break;
		case K_EQUAL:				fxEqualExpressionStep(parser);	break;
		case K_RELATIONAL:			fxRelationalExpressionStep(parser);	break;
		case K_SHIFT:				fxShiftExpressionStep(parser);	break;
		case K_ADDITIVE:			fxAdditiveExpressionStep(parser);	break;
		case K_MULTIPLICATIVE:		fxMultiplicativeExpressionStep(parser);	break;
		case K_EXPONENTIATION:		fxExponentiationExpressionStep(parser);	break;
		case K_UNARY:				fxUnaryExpressionStep(parser);	break;
		case K_PREFIX:				fxPrefixExpressionStep(parser);	break;
		case K_POSTFIX:				fxPostfixExpressionStep(parser);	break;
		case K_CALL_CHAIN:			fxCallExpressionStep(parser);	break;
		case K_LITERAL:				fxLiteralExpressionStep(parser);	break;
		case K_ARRAY_EXPR:			fxArrayExpressionStep(parser);	break;
		case K_ARROW:				fxArrowExpressionStep(parser);	break;
		case K_CLASS_EXPR:			fxClassExpressionStep(parser);	break;
		case K_FUNCTION_EXPR:		fxFunctionExpressionStep(parser);	break;
		case K_GENERATOR_EXPR:		fxGeneratorExpressionStep(parser);	break;
		case K_GROUP_EXPR:			fxGroupExpressionStep(parser);	break;
		case K_NEW_EXPR:			fxNewExpressionStep(parser);	break;
		case K_OBJECT_EXPR:			fxObjectExpressionStep(parser);	break;
		case K_TEMPLATE_EXPR:		fxTemplateExpressionStep(parser);	break;
		case K_YIELD_EXPR:			fxYieldExpressionStep(parser);	break;
		case K_PARAMETERS:			fxParametersStep(parser);		break;
		case K_BODY:				fxBodyStep(parser);			break;
		case K_PROPERTY_NAME:		fxPropertyNameStep(parser);	break;
		case K_BINDING:				fxBindingStep(parser);			break;
		case K_ARRAY_BINDING:		fxArrayBindingStep(parser);	break;
		case K_OBJECT_BINDING:		fxObjectBindingStep(parser);	break;
		case K_PARAMETERS_BINDING:	fxParametersBindingStep(parser);	break;
		case K_REST_BINDING:		fxRestBindingStep(parser);		break;
		case K_JSON_VALUE:			fxJSONValueStep(parser);		break;
		case K_JSON_OBJECT:			fxJSONObjectStep(parser);		break;
		case K_JSON_ARRAY:			fxJSONArrayStep(parser);		break;
		case K_JSX_ELEMENT:			fxJSXElementStep(parser);		break;
		case K_JSX_ATTRIBUTE_VALUE:	fxJSXAttributeValueStep(parser);	break;
		case K_CHECK_STRICT_BINDING:	fxCheckStrictBindingStep(parser);	break;
		case K_PARAMS_BINDING_FROM:		fxParametersBindingFromStep(parser);	break;
		case K_PROGRAM:				fxProgramStep(parser);			break;
		case K_STATEMENTS:			fxStatementsStep(parser);		break;
		case K_STATEMENT:			fxStatementStep(parser);		break;
		default:
#ifdef XS_NR_PUMP_TRACE
			c_fprintf(stderr, "UNKNOWN KIND %d pc=%d\n", (int)kind, (int)parser->curFrame->pc);
#endif
			fxAbort(parser->console, XS_NATIVE_STACK_OVERFLOW_EXIT);
			break;
	}
}

/* xs_no_recursion R22: budget gate shared by every pump loop. When a
   stepwise parse is armed (pumpBudget > 0), the loop returns once
   pumpSteps reaches the budget with frames still pending — the parse
   is PAUSED and fully resumable (all parse state lives in the parser:
   frame chain, token stream, states). The driver clears pumpPaused and
   resets pumpSteps before resuming. pumpBudget == 0 (the default)
   never fires: run-to-completion, byte-for-byte the old behavior. */
#define P_BUDGET_PAUSE(parser) \
	((parser)->pumpBudget && \
	 (++(parser)->pumpSteps >= (parser)->pumpBudget) ? \
	 ((parser)->pumpPaused = 1, (parser)->pumpRunning = 0, 1) : 0)

void fxParserPump(txParser* parser)
{
	/* xs_no_recursion R22: the stepwise driver's resume pump. Mark the
	   pump active so nested fxParserCall* calls only PUSH frames (their
	   pumpRunning guard skips their own loops) — every executed step then
	   runs in THIS loop, so a budget pause can only fire here. An inner
	   pump pause would return into a C caller that assumes its sub-parse
	   completed (the C callers between here and the driver are not
	   resumable). Restored on normal completion; P_BUDGET_PAUSE clears
	   it when pausing. */
	int savedRunning = parser->pumpRunning;
	parser->pumpRunning = 1;
	while (parser->curFrame) {
		fxParserRunStep(parser, parser->curFrame->kind);
		if (P_BUDGET_PAUSE(parser))
			return;
	}
	parser->pumpRunning = savedRunning;
}

void fxParserCall(txParser* parser, txInteger kind, txInteger line)
{
	txParserFrame* baseFrame = parser->curFrame;
	fxParserFramePush(parser, kind, line);
#ifdef XS_NR_PUMP_TRACE
	c_fprintf(stderr, "PUSH kind=%d line=%u\n", (int)kind, (unsigned)line);
#endif
	if (parser->pumpRunning == 0) {
		unsigned long steps = 0;
		parser->pumpRunning = 1;
		while (parser->curFrame != baseFrame) {
			fxParserRunStep(parser, parser->curFrame->kind);
			if (P_BUDGET_PAUSE(parser))
				return;
#ifdef XS_NR_PUMP_TRACE
			if (++steps % 100000 == 0)
				c_fprintf(stderr, "pump kind=%d steps=%lu topkind=%d topPc=%d\n",
					(int)kind, steps, (int)parser->curFrame->kind, (int)parser->curFrame->pc);
#endif
		}
#ifdef XS_NR_PUMP_TRACE
		c_fprintf(stderr, "PUMPEXIT base=%p top=%p\n", (void*)baseFrame, (void*)parser->curFrame);
#endif
		parser->pumpRunning = 0;
	}
}

void fxParserCallParam(txParser* parser, txInteger kind, txInteger line, txInteger i0, txUnsigned u0)
{
	txParserFrame* baseFrame = parser->curFrame;
	fxParserFramePush(parser, kind, line);
#ifdef XS_NR_PUMP_TRACE
	c_fprintf(stderr, "PUSH kind=%d line=%u pump=%d f=%p base=%p\n", (int)kind, (unsigned)line, (int)parser->pumpRunning, (void*)parser->curFrame, (void*)baseFrame);
#endif
	parser->curFrame->i0 = i0;
	parser->curFrame->u0 = u0;
	if (parser->pumpRunning == 0) {
		parser->pumpRunning = 1;
		parser->pumpBase = baseFrame;
		parser->pumpBase = baseFrame;
		parser->pumpBase = baseFrame;
		unsigned long steps = 0;
		while (parser->curFrame != baseFrame) {
			fxParserRunStep(parser, parser->curFrame->kind);
			if (P_BUDGET_PAUSE(parser))
				return;
#ifdef XS_NR_PUMP_TRACE
			if (++steps % 100000 == 0)
				c_fprintf(stderr, "pump %s steps=%lu topkind=%d topPc=%d line=%u\n", __func__, steps, (int)parser->curFrame->kind, (int)parser->curFrame->pc, (unsigned)parser->states[0].line);
#endif
		}
		parser->pumpRunning = 0;
	}
}

void fxParserCallFlag(txParser* parser, txInteger kind, txInteger line, txUnsigned u0)
{
	txParserFrame* baseFrame = parser->curFrame;
	fxParserFramePush(parser, kind, line);
#ifdef XS_NR_PUMP_TRACE
	c_fprintf(stderr, "PUSH kind=%d line=%u\n", (int)kind, (unsigned)line);
#endif
	parser->curFrame->u0 = u0;
	/* xs_no_recursion: mirror the flag into u2 — function/generator/arrow
	   steps read their flag argument from u2 (CallTokenFlag layout), and
	   no other CallFlag target reads u0 and u2 differently. */
	parser->curFrame->u2 = u0;
	if (parser->pumpRunning == 0) {
		parser->pumpRunning = 1;
		parser->pumpBase = baseFrame;
		unsigned long steps = 0;
		while (parser->curFrame != baseFrame) {
			fxParserRunStep(parser, parser->curFrame->kind);
			if (P_BUDGET_PAUSE(parser))
				return;
#ifdef XS_NR_PUMP_TRACE
			if (++steps % 100000 == 0)
				c_fprintf(stderr, "pump %s steps=%lu topkind=%d topPc=%d line=%u\n", __func__, steps, (int)parser->curFrame->kind, (int)parser->curFrame->pc, (unsigned)parser->states[0].line);
#endif
		}
		parser->pumpRunning = 0;
	}
}

void fxParserCallTokenFlag(txParser* parser, txInteger kind, txInteger line, txToken t0, txUnsigned u0)
{
	txParserFrame* baseFrame = parser->curFrame;
	fxParserFramePush(parser, kind, line);
#ifdef XS_NR_PUMP_TRACE
	c_fprintf(stderr, "PUSH kind=%d line=%u\n", (int)kind, (unsigned)line);
#endif
	parser->curFrame->t1 = t0;
	parser->curFrame->u2 = u0;
	if (parser->pumpRunning == 0) {
		parser->pumpRunning = 1;
		parser->pumpBase = baseFrame;
		unsigned long steps = 0;
		while (parser->curFrame != baseFrame) {
			fxParserRunStep(parser, parser->curFrame->kind);
			if (P_BUDGET_PAUSE(parser))
				return;
#ifdef XS_NR_PUMP_TRACE
			if (++steps % 100000 == 0)
				c_fprintf(stderr, "pump %s steps=%lu topkind=%d topPc=%d line=%u\n", __func__, steps, (int)parser->curFrame->kind, (int)parser->curFrame->pc, (unsigned)parser->states[0].line);
#endif
		}
		parser->pumpRunning = 0;
	}
}

void fxParserCallNode(txParser* parser, txInteger kind, txInteger line, txNode* n0)
{
	txParserFrame* baseFrame = parser->curFrame;
	fxParserFramePush(parser, kind, line);
#ifdef XS_NR_PUMP_TRACE
	c_fprintf(stderr, "PUSH kind=%d line=%u\n", (int)kind, (unsigned)line);
#endif
	parser->curFrame->n0 = n0;
	if (parser->pumpRunning == 0) {
		parser->pumpRunning = 1;
		parser->pumpBase = baseFrame;
		unsigned long steps = 0;
		while (parser->curFrame != baseFrame) {
			fxParserRunStep(parser, parser->curFrame->kind);
			if (P_BUDGET_PAUSE(parser))
				return;
#ifdef XS_NR_PUMP_TRACE
			if (++steps % 100000 == 0)
				c_fprintf(stderr, "pump %s steps=%lu topkind=%d topPc=%d line=%u\n", __func__, steps, (int)parser->curFrame->kind, (int)parser->curFrame->pc, (unsigned)parser->states[0].line);
#endif
		}
		parser->pumpRunning = 0;
	}
}

/* xs_no_recursion: fxClassExpression(parser, line, &symbol) equivalent —
   pushes theLine via i1 and flag via u2 (the slots the steps read); the
   name is read back from parser->outSymbol after the pump drains
   (fxParserReturn publishes it when the frame pops). */
void fxParserCallClassFlag(txParser* parser, txInteger kind, txInteger line, txUnsigned u0, txSymbol** theSymbol)
{
	txParserFrame* baseFrame = parser->curFrame;
	fxParserFramePush(parser, kind, line);
#ifdef XS_NR_PUMP_TRACE
	c_fprintf(stderr, "PUSH kind=%d line=%u\n", (int)kind, (unsigned)line);
#endif
	parser->curFrame->i1 = line;
	parser->curFrame->u2 = u0;
	if (parser->pumpRunning == 0) {
		parser->pumpRunning = 1;
		while (parser->curFrame != baseFrame) {
			fxParserRunStep(parser, parser->curFrame->kind);
			if (P_BUDGET_PAUSE(parser))
				return;
		}
		parser->pumpRunning = 0;
	}
	if (theSymbol)
		*theSymbol = parser->outSymbol;
}




void fxCheckStrictBindingStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0: {
		txNode* node = f->n0;
		if (!node || !node->description)
			break;
		switch (node->description->token) {
		case XS_TOKEN_ACCESS:
			fxCheckStrictSymbol(parser, ((txAccessNode*)node)->symbol);
			break;
		case XS_TOKEN_ARG:
		case XS_TOKEN_CONST:
		case XS_TOKEN_LET:
		case XS_TOKEN_USING:
		case XS_TOKEN_VAR:
			fxCheckStrictSymbol(parser, ((txDeclareNode*)node)->symbol);
			break;
		case XS_TOKEN_BINDING:
			/* xs_no_recursion: CALL pushes the child and returns; falling into
			   fxParserReturn here would pop the CHILD (stack top) instead of
			   this frame, and the pump would re-run case 0 forever. Resume at
			   the empty case 1 so this frame pops only after the child is
			   done (same pattern as every other machine's CALL + P_RESUME). */
			fxParserCallNode(parser, K_CHECK_STRICT_BINDING, parser->states[0].line, ((txBindingNode*)node)->target);
			P_RESUME(1);
		case XS_TOKEN_ARRAY_BINDING:
			f->n1 = ((txArrayBindingNode*)node)->items->first;
			f->pc = 2;
			return;
		case XS_TOKEN_OBJECT_BINDING:
			f->n1 = ((txObjectBindingNode*)node)->items->first;
			f->pc = 2;
			return;
		case XS_TOKEN_PARAMS_BINDING:
			f->n1 = ((txParamsBindingNode*)node)->items->first;
			f->pc = 2;
			return;
		case XS_TOKEN_PROPERTY_BINDING:
			fxParserCallNode(parser, K_CHECK_STRICT_BINDING, parser->states[0].line, ((txPropertyBindingNode*)node)->binding);
			P_RESUME(1);
			break;
		case XS_TOKEN_PROPERTY_BINDING_AT:
			fxParserCallNode(parser, K_CHECK_STRICT_BINDING, parser->states[0].line, ((txPropertyBindingAtNode*)node)->binding);
			P_RESUME(1);
			break;
		case XS_TOKEN_REST_BINDING:
			fxParserCallNode(parser, K_CHECK_STRICT_BINDING, parser->states[0].line, ((txRestBindingNode*)node)->binding);
			P_RESUME(1);
			break;
		}
		break;
	}
	case 1:
		break;
	case 2: {
		/* sibling iteration: f->n1 is the current item */
		txNode* node = f->n1;
		if (!node)
			break;
		f->n1 = node->next;
		fxParserCallNode(parser, K_CHECK_STRICT_BINDING, parser->states[0].line, node);
		f->pc = 2;
		return;
	}
	}
	fxParserReturn(parser);
}

void fxCommaExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
#ifdef XS_NR_PUMP_TRACE
		c_fprintf(stderr, "COMMA tok=%d (%s)\n", (int)parser->states[0].token, gxTokenNames[parser->states[0].token]);
#endif
		f->i0 = parser->states[0].line;
		f->i1 = 0;
		if (!(gxTokenFlags[parser->states[0].token] & XS_TOKEN_BEGIN_EXPRESSION)) {
			fxPushNULL(parser);
			fxReportParserError(parser, parser->states[0].line, "missing expression");
			break;
		}
		fxParserCall(parser, K_ASSIGN, parser->states[0].line);
		P_RESUME(1);
	case 1:
		f->i1++;
		while (parser->states[0].token == XS_TOKEN_COMMA) {
			fxGetNextToken(parser);
			fxParserCall(parser, K_ASSIGN, parser->states[0].line);
			P_RESUME(2);
		}
		if (f->i1 > 1) {
			fxPushNodeList(parser, f->i1);
			fxPushNodeStruct(parser, 1, XS_TOKEN_EXPRESSIONS, f->i0);
		}
		break;
	case 2:
		/* xs_no_recursion: no f->i1++ here; case 1 increments once per child
		   on re-entry (stock fxCommaExpression counts each child exactly once) */
		f->pc = 1;
		return;
	}
	fxParserReturn(parser);
}

void fxLiteralExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		/* f->u0 holds the flag argument passed by the caller */
		f->i1 = 0;
		f->t1 = XS_NO_TOKEN;
		f->i0 = parser->states[0].line;
		f->u1 = 0;
		fxCheckParserStack(parser, f->i0);
		switch (parser->states[0].token) {
		case XS_TOKEN_NULL:
		case XS_TOKEN_TRUE:
		case XS_TOKEN_FALSE:
			fxPushNodeStruct(parser, 0, parser->states[0].token, f->i0);
			fxMatchToken(parser, parser->states[0].token);
			break;
		case XS_TOKEN_IMPORT:
			fxMatchToken(parser, XS_TOKEN_IMPORT);
			if (!f->u0 && (parser->states[0].token == XS_TOKEN_LEFT_PARENTHESIS)) {
				f->u2 = parser->flags & mxForFlag;
				parser->flags &= ~mxForFlag;
				fxParserCall(parser, K_ASSIGN, parser->states[0].line);
				P_RESUME(1);
			}
			else if (parser->states[0].token == XS_TOKEN_DOT) {
				fxGetNextToken(parser);
				if ((parser->states[0].token == XS_TOKEN_IDENTIFIER) && (parser->states[0].symbol == parser->metaSymbol) && (!parser->states[0].escaped)) {
					fxGetNextToken(parser);
					if (parser->flags & mxProgramFlag)
						fxReportParserError(parser, parser->states[0].line, "invalid import.meta");
					else
						fxPushNodeStruct(parser, 0, XS_TOKEN_IMPORT_META, f->i0);
				}
				else
					fxReportParserError(parser, parser->states[0].line, "invalid import.");
				break;
			}
			else
				fxReportParserError(parser, parser->states[0].line, "invalid import");
			break;
		case XS_TOKEN_SUPER:
			fxMatchToken(parser, XS_TOKEN_SUPER);
			if (parser->states[0].token == XS_TOKEN_LEFT_PARENTHESIS) {
				if (parser->flags & mxDerivedFlag) {
					fxParserCall(parser, K_PARAMETERS, parser->states[0].line);
					P_RESUME(2);
				}
				else {
					fxPushNodeStruct(parser, 0, XS_TOKEN_UNDEFINED, f->i0);
					fxReportParserError(parser, parser->states[0].line, "invalid super");
				}
			}
			else if ((parser->states[0].token == XS_TOKEN_DOT) || (parser->states[0].token == XS_TOKEN_LEFT_BRACKET)) {
				if (parser->flags & mxSuperFlag) {
					fxPushNodeStruct(parser, 0, XS_TOKEN_THIS, f->i0);
					parser->root->flags |= parser->flags & (mxDerivedFlag | mxSuperFlag);
				}
				else {
					fxPushNodeStruct(parser, 0, XS_TOKEN_UNDEFINED, f->i0);
					fxReportParserError(parser, parser->states[0].line, "invalid super");
				}
			}
			else
				fxReportParserError(parser, parser->states[0].line, "invalid super");
			parser->flags |= mxSuperFlag;
			break;
		case XS_TOKEN_THIS:
			fxPushNodeStruct(parser, 0, parser->states[0].token, f->i0);
			parser->root->flags |= parser->flags & mxDerivedFlag;
			fxMatchToken(parser, XS_TOKEN_THIS);
			break;
		case XS_TOKEN_INTEGER:
			fxPushIntegerNode(parser, parser->states[0].integer, f->i0);
			fxGetNextToken(parser);
			break;
		case XS_TOKEN_NUMBER:
			fxPushNumberNode(parser, parser->states[0].number, f->i0);
			fxGetNextToken(parser);
			break;
		case XS_TOKEN_BIGINT:
			fxPushBigIntNode(parser, &parser->states[0].bigint, f->i0);
			fxGetNextToken(parser);
			break;
		case XS_TOKEN_DIVIDE_ASSIGN:
		case XS_TOKEN_DIVIDE: {
			char c = (parser->states[0].token == XS_TOKEN_DIVIDE_ASSIGN) ? '=' : 0;
			fxGetNextRegExp(parser, c);
			fxPushStringNode(parser, parser->states[0].modifierLength, parser->states[0].modifier, f->i0);
			fxPushStringNode(parser, parser->states[0].stringLength, parser->states[0].string, f->i0);
			fxPushNodeStruct(parser, 2, XS_TOKEN_REGEXP, f->i0);
			fxGetNextToken(parser);
			break;
		}
		case XS_TOKEN_STRING:
			fxPushStringNode(parser, parser->states[0].stringLength, parser->states[0].string, f->i0);
			fxGetNextToken(parser);
			break;
		case XS_TOKEN_IDENTIFIER: {
			int escaped = parser->states[0].escaped;
			f->s0 = parser->states[0].symbol;
			fxGetNextToken(parser);
			if ((f->s0 == parser->asyncSymbol) && (!escaped) && (!parser->states[0].crlf)) {
				if (parser->states[0].token == XS_TOKEN_FUNCTION) {
					fxMatchToken(parser, XS_TOKEN_FUNCTION);
					if (parser->states[0].token == XS_TOKEN_MULTIPLY) {
						fxGetNextToken(parser);
						fxParserCallFlag(parser, K_GENERATOR_EXPR, f->i0, mxAsyncFlag);
						P_RESUME(4);
					}
					else {
						fxParserCallFlag(parser, K_FUNCTION_EXPR, f->i0, mxAsyncFlag);
						P_RESUME(4);
					}
				}
				if (parser->states[0].token == XS_TOKEN_LEFT_PARENTHESIS) {
					fxParserCallFlag(parser, K_GROUP_EXPR, parser->states[0].line, mxAsyncFlag);
					P_RESUME(4);
				}
				if (parser->states[0].token == XS_TOKEN_IDENTIFIER) {
					f->s0 = parser->states[0].symbol;
					fxGetNextToken(parser);
					f->u1 = mxAsyncFlag;
				}
			}
			if (f->s0 == parser->awaitSymbol)
				parser->flags |= mxAwaitingFlag;
			f->i2 = 0;
			if ((!parser->states[0].crlf) && (parser->states[0].token == XS_TOKEN_ARROW)) {
				fxCheckStrictSymbol(parser, f->s0);
				if (f->u1 && (f->s0 == parser->awaitSymbol))
					fxReportParserError(parser, parser->states[0].line, "invalid await");
				fxPushSymbol(parser, f->s0);
				fxPushNULL(parser);
				fxPushNodeStruct(parser, 2, XS_TOKEN_ARG, f->i0);
				fxPushNodeList(parser, 1);
				fxPushNodeStruct(parser, 1, XS_TOKEN_PARAMS_BINDING, f->i0);
				fxParserCallFlag(parser, K_ARROW, f->i0, f->u1);
				P_RESUME(4);
			}
			if (f->s0 == parser->argumentsSymbol)
				parser->flags |= mxArgumentsFlag;
			fxPushSymbol(parser, f->s0);
			fxPushNodeStruct(parser, 1, XS_TOKEN_ACCESS, f->i0);
			break;
		}
		case XS_TOKEN_CLASS: {
			txUnsigned flags = parser->flags & mxForFlag;
			parser->flags &= ~mxForFlag;
			f->u2 = flags;
			fxParserCallFlag(parser, K_CLASS_EXPR, f->i0, 0);
			P_RESUME(5);
		}
		case XS_TOKEN_FUNCTION:
			fxMatchToken(parser, XS_TOKEN_FUNCTION);
			if (parser->states[0].token == XS_TOKEN_MULTIPLY) {
				fxGetNextToken(parser);
				fxParserCallFlag(parser, K_GENERATOR_EXPR, f->i0, 0);
				P_RESUME(4);
			}
			else {
				fxParserCallFlag(parser, K_FUNCTION_EXPR, f->i0, 0);
				P_RESUME(4);
			}
		case XS_TOKEN_NEW:
			fxParserCall(parser, K_NEW_EXPR, f->i0);
			P_RESUME(4);
		case XS_TOKEN_LEFT_BRACE: {
			txUnsigned flags = parser->flags & mxForFlag;
			parser->flags &= ~mxForFlag;
			f->u2 = flags;
			fxParserCall(parser, K_OBJECT_EXPR, f->i0);
			P_RESUME(5);
		}
		case XS_TOKEN_LEFT_BRACKET: {
			txUnsigned flags = parser->flags & mxForFlag;
			parser->flags &= ~mxForFlag;
			f->u2 = flags;
			fxParserCall(parser, K_ARRAY_EXPR, f->i0);
			P_RESUME(5);
		}
		case XS_TOKEN_LEFT_PARENTHESIS:
			fxParserCallFlag(parser, K_GROUP_EXPR, f->i0, 0);
			P_RESUME(4);
		case XS_TOKEN_TEMPLATE:
			fxPushNULL(parser);
			fxPushStringNode(parser, parser->states[0].stringLength, parser->states[0].string, f->i0);
			fxPushRawNode(parser, parser->states[0].rawLength, parser->states[0].raw, f->i0);
			fxPushNodeStruct(parser, 2, XS_TOKEN_TEMPLATE_MIDDLE, f->i0);
			fxGetNextToken(parser);
			fxPushNodeList(parser, 1);
			fxPushNodeStruct(parser, 2, XS_TOKEN_TEMPLATE, f->i0);
			break;
		case XS_TOKEN_TEMPLATE_HEAD:
			fxPushNULL(parser);
			fxParserCall(parser, K_TEMPLATE_EXPR, f->i0);
			P_RESUME(6);
		case XS_TOKEN_HOST:
			fxGetNextToken(parser);
			fxPushNULL(parser);
			fxPushNULL(parser);
			if (parser->states[0].token == XS_TOKEN_STRING) {
				fxPushStringNode(parser, parser->states[0].stringLength, parser->states[0].string, f->i0);
				fxGetNextToken(parser);
			}
			else {
				fxReportParserError(parser, parser->states[0].line, "invalid host object");
				fxPushNULL(parser);
			}
			fxPushNodeStruct(parser, 3, XS_TOKEN_HOST, f->i0);
			break;
		case XS_TOKEN_LESS:
			fxGetNextToken(parser);
			fxParserCall(parser, K_JSX_ELEMENT, f->i0);
			P_RESUME(7);
		default:
			fxPushNodeStruct(parser, 0, XS_TOKEN_UNDEFINED, f->i0);
			fxReportParserError(parser, parser->states[0].line, "missing expression");
			break;
		}
		break;
	case 1:
		if (parser->states[0].token == XS_TOKEN_COMMA) {
			fxGetNextToken(parser);
			if (gxTokenFlags[parser->states[0].token] & XS_TOKEN_BEGIN_EXPRESSION) {
				fxParserCall(parser, K_ASSIGN, parser->states[0].line);
				P_RESUME(8);
			}
			else
				fxPushNULL(parser);
		}
		else
			fxPushNULL(parser);
		parser->flags |= f->u2;
		fxMatchToken(parser, XS_TOKEN_RIGHT_PARENTHESIS);
		fxPushNodeStruct(parser, 2, XS_TOKEN_IMPORT_CALL, f->i0);
		break;
	case 8:
		if (parser->states[0].token == XS_TOKEN_COMMA)
			fxGetNextToken(parser);
		parser->flags |= f->u2;
		fxMatchToken(parser, XS_TOKEN_RIGHT_PARENTHESIS);
		fxPushNodeStruct(parser, 2, XS_TOKEN_IMPORT_CALL, f->i0);
		break;
	case 2:
		fxPushNodeStruct(parser, 1, XS_TOKEN_SUPER, f->i0);
		break;
	case 4:
		/* shared exit for rules that leave the node stack ready */
		break;
	case 5:
		parser->flags |= f->u2;
		break;
	case 6:
		fxPushNodeStruct(parser, 2, XS_TOKEN_TEMPLATE, f->i0);
		break;
	case 7:
		fxGetNextToken(parser);
		break;
	}
	fxParserReturn(parser);
}

void fxGroupExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		/* f->u0 holds the flag argument passed by the caller */
		f->b0 = 0;
		f->b1 = 0;
		f->i1 = 0;
		f->u1 = parser->flags & (mxAwaitingFlag | mxYieldingFlag);
		parser->flags &= ~(mxAwaitingFlag | mxYieldingFlag);
		fxMatchToken(parser, XS_TOKEN_LEFT_PARENTHESIS);
		f->pc = 1;
		return;
	case 1:
		if (!((parser->states[0].token == XS_TOKEN_SPREAD) || (gxTokenFlags[parser->states[0].token] & XS_TOKEN_BEGIN_EXPRESSION))) {
			f->pc = 2;
			return;
		}
		f->i2 = parser->states[0].line;
		f->b0 = 0;
		if (parser->states[0].token == XS_TOKEN_SPREAD) {
			fxGetNextToken(parser);
			fxParserCall(parser, K_ASSIGN, parser->states[0].line);
			P_RESUME(3);
		}
		fxParserCall(parser, K_ASSIGN, parser->states[0].line);
		P_RESUME(4);
	case 2:
		f->i0 = parser->states[0].line;
		fxMatchToken(parser, XS_TOKEN_RIGHT_PARENTHESIS);
		if ((!parser->states[0].crlf) && (parser->states[0].token == XS_TOKEN_ARROW)) {
			fxPushNodeList(parser, f->i1);
			fxPushNodeStruct(parser, 1, XS_TOKEN_EXPRESSIONS, f->i0);
			if (f->b0 && f->b1)
				fxReportParserError(parser, parser->states[0].line, "invalid parameters");
			fxParserCall(parser, K_PARAMS_BINDING_FROM, parser->states[0].line);
			P_RESUME(5);
		}
		else if (f->u0) {
			fxPushNodeList(parser, f->i1);
			fxPushNodeStruct(parser, 1, XS_TOKEN_PARAMS, f->i0);
			if (f->b1)
				parser->root->flags |= mxSpreadFlag;
			fxPushSymbol(parser, parser->asyncSymbol);
			fxPushNodeStruct(parser, 1, XS_TOKEN_ACCESS, f->i0);
			fxSwapNodes(parser);
			fxPushNodeStruct(parser, 2, XS_TOKEN_CALL, f->i0);
			parser->flags |= f->u1;
			break;
		}
		else {
			if ((f->i1 == 0) || f->b0) {
				fxPushNULL(parser);
				fxReportParserError(parser, parser->states[0].line, "missing expression");
			}
			else {
				fxPushNodeList(parser, f->i1);
				fxPushNodeStruct(parser, 1, XS_TOKEN_EXPRESSIONS, f->i0);
			}
			parser->flags |= f->u1;
			break;
		}
	case 3:
		fxPushNodeStruct(parser, 1, XS_TOKEN_SPREAD, f->i2);
		f->b1 = 1;
		f->i1++;
		if (parser->states[0].token != XS_TOKEN_COMMA) {
			f->pc = 2;
			return;
		}
		fxGetNextToken(parser);
		f->b0 = 1;
		f->pc = 1;
		return;
	case 4:
		f->i1++;
		if (parser->states[0].token != XS_TOKEN_COMMA) {
			f->pc = 2;
			return;
		}
		fxGetNextToken(parser);
		f->b0 = 1;
		f->pc = 1;
		return;
	case 5:
		if (!parser->outFlags)
			fxReportParserError(parser, parser->states[0].line, "no parameters");
		fxParserCallNode(parser, K_CHECK_STRICT_BINDING, parser->states[0].line, parser->root);
		P_RESUME(6);
	case 6:
		parser->root->flags |= f->u0;
		if (parser->flags & mxAwaitingFlag) {
			if (f->u0 || (parser->flags & mxAsyncFlag))
				fxReportParserError(parser, parser->states[0].line, "invalid await");
			else
				f->u1 |= mxAwaitingFlag;
		}
		if (parser->flags & mxYieldingFlag)
			fxReportParserError(parser, parser->states[0].line, "invalid yield");
		fxParserCallFlag(parser, K_ARROW, f->i0, f->u0);
		P_RESUME(7);
	case 7:
		parser->flags |= f->u1;
		break;
	}
	fxParserReturn(parser);
}

void fxNewExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->i0 = parser->states[0].line;
		fxMatchToken(parser, XS_TOKEN_NEW);
		if (parser->states[0].token == XS_TOKEN_DOT) {
			fxGetNextToken(parser);
			if (fxIsKeyword(parser, parser->targetSymbol)) {
				if (!(parser->flags & mxTargetFlag))
					fxReportParserError(parser, parser->states[0].line, "invalid new.target");
				fxGetNextToken(parser);
				fxPushNodeStruct(parser, 0, XS_TOKEN_TARGET, f->i0);
			}
			else
				fxReportParserError(parser, parser->states[0].line, "missing target");
			break;
		}
		fxParserCallFlag(parser, K_LITERAL, f->i0, 1);
		P_RESUME(1);
	case 1:
		fxCheckArrowFunction(parser, 1);
		f->pc = 2;
		return;
	case 2:
		f->i1 = parser->states[0].line;
		if (parser->states[0].token == XS_TOKEN_DOT) {
			fxGetNextToken(parser);
			if (parser->states[0].token == XS_TOKEN_IDENTIFIER) {
				fxPushSymbol(parser, parser->states[0].symbol);
				fxPushNodeStruct(parser, 2, XS_TOKEN_MEMBER, f->i1);
				fxGetNextToken(parser);
			}
			else if (parser->states[0].token == XS_TOKEN_PRIVATE_IDENTIFIER) {
				fxPushSymbol(parser, parser->states[0].symbol);
				fxSwapNodes(parser);
				fxPushNodeStruct(parser, 2, XS_TOKEN_PRIVATE_MEMBER, f->i1);
				fxGetNextToken(parser);
			}
			else
				fxReportParserError(parser, parser->states[0].line, "missing property");
			f->pc = 2;
			return;
		}
		else if (parser->states[0].token == XS_TOKEN_LEFT_BRACKET) {
			fxGetNextToken(parser);
			fxParserCall(parser, K_COMMA, parser->states[0].line);
			P_RESUME(3);
		}
		else if (parser->states[0].token == XS_TOKEN_TEMPLATE) {
			fxPushStringNode(parser, parser->states[0].stringLength, parser->states[0].string, f->i0);
			fxPushRawNode(parser, parser->states[0].rawLength, parser->states[0].raw, f->i0);
			fxPushNodeStruct(parser, 2, XS_TOKEN_TEMPLATE_MIDDLE, f->i0);
			fxGetNextToken(parser);
			fxPushNodeList(parser, 1);
			fxPushNodeStruct(parser, 2, XS_TOKEN_TEMPLATE, f->i0);
			f->pc = 2;
			return;
		}
		else if (parser->states[0].token == XS_TOKEN_TEMPLATE_HEAD) {
			fxParserCall(parser, K_TEMPLATE_EXPR, f->i0);
			P_RESUME(4);
		}
		else
			f->pc = 5;
		return;
	case 3:
		fxPushNodeStruct(parser, 2, XS_TOKEN_MEMBER_AT, f->i1);
		fxMatchToken(parser, XS_TOKEN_RIGHT_BRACKET);
		f->pc = 2;
		return;
	case 4:
		fxPushNodeStruct(parser, 2, XS_TOKEN_TEMPLATE, f->i1);
		f->pc = 2;
		return;
	case 5:
		if (parser->states[0].token == XS_TOKEN_LEFT_PARENTHESIS) {
			fxParserCall(parser, K_PARAMETERS, parser->states[0].line);
			P_RESUME(6);
		}
		else {
			fxPushNodeList(parser, 0);
			fxPushNodeStruct(parser, 1, XS_TOKEN_PARAMS, f->i0);
			fxPushNodeStruct(parser, 2, XS_TOKEN_NEW, f->i0);
		}
		break;
	case 6:
		fxPushNodeStruct(parser, 2, XS_TOKEN_NEW, f->i0);
		break;
	}
	fxParserReturn(parser);
}

void fxStatementsStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->i1 = parser->nodeCount;
		f->i0 = parser->states[0].line;
		f->pc = 1;
		return;
	case 1:
		if ((parser->states[0].token == XS_TOKEN_EOF) || (parser->states[0].token == XS_TOKEN_RIGHT_BRACE)) {
			fxPushNodeList(parser, parser->nodeCount - f->i1);
			fxPushNodeStruct(parser, 1, XS_TOKEN_STATEMENTS, f->i0);
			break;
		}
		fxParserCallParam(parser, K_STATEMENT, parser->states[0].line, 1, 0);
		P_RESUME(1);
	}
	fxParserReturn(parser);
}

void fxStatementStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->blockIt = f->i0;
		f->i0 = parser->states[0].line;
		f->s0 = C_NULL;
		f->u0 = 0;
		switch (parser->states[0].token) {
		case XS_TOKEN_SEMICOLON:
			fxGetNextToken(parser);
			if (!f->blockIt) {
				fxPushNodeStruct(parser, 0, XS_TOKEN_UNDEFINED, f->i0);
				fxPushNodeStruct(parser, 1, XS_TOKEN_STATEMENT, f->i0);
			}
			break;
		case XS_TOKEN_BREAK:
			fxBreakStatement(parser);
			fxSemicolon(parser);
			break;
		case XS_TOKEN_CLASS:
			if (!f->blockIt)
				fxReportParserError(parser, parser->states[0].line, "no block");
			f->s0 = C_NULL;
			fxParserCallFlag(parser, K_CLASS_EXPR, f->i0, 0);
			P_RESUME(10);
		case XS_TOKEN_CONST:
			if (!f->blockIt)
				fxReportParserError(parser, parser->states[0].line, "no block");
			f->t0 = XS_TOKEN_CONST;
			fxParserCallTokenFlag(parser, K_VARIABLE_STATEMENT, f->i0, f->t0, 0);
			P_RESUME(11);
		case XS_TOKEN_CONTINUE:
			fxContinueStatement(parser);
			fxSemicolon(parser);
			break;
		case XS_TOKEN_DEBUGGER:
			fxDebuggerStatement(parser);
			fxSemicolon(parser);
			break;
		case XS_TOKEN_DO:
			fxParserCall(parser, K_DO, f->i0);
			P_RESUME(12);
		case XS_TOKEN_FOR:
			fxParserCall(parser, K_FOR, f->i0);
			P_RESUME(12);
		case XS_TOKEN_FUNCTION:
		again:
			if (!f->blockIt)
				fxReportParserError(parser, parser->states[0].line, "no block (strict code)");
			fxMatchToken(parser, XS_TOKEN_FUNCTION);
			if (parser->states[0].token == XS_TOKEN_MULTIPLY) {
				fxGetNextToken(parser);
				f->u0 = 0;
				fxParserCallFlag(parser, K_GENERATOR_EXPR, f->i0, f->u0);
			}
			else
				fxParserCallFlag(parser, K_FUNCTION_EXPR, f->i0, f->u0);
			P_RESUME(1);
		case XS_TOKEN_IF:
			fxParserCall(parser, K_IF, f->i0);
			P_RESUME(12);
		case XS_TOKEN_RETURN:
			if (!(parser->flags & (mxArrowFlag | mxFunctionFlag | mxGeneratorFlag)))
				fxReportParserError(parser, parser->states[0].line, "invalid return");
			/* xs_no_recursion: fxReturnStatement's expression sub-call is a
			   trampoline forwarder (async inside the pump), so inline the
			   statement body across outer pcs 14/15. */
			f->i1 = parser->states[0].line;
			fxMatchToken(parser, XS_TOKEN_RETURN);
			if ((!parser->states[0].crlf) && (gxTokenFlags[parser->states[0].token] & XS_TOKEN_BEGIN_EXPRESSION)) {
				fxParserCall(parser, K_COMMA, f->i1);
				P_RESUME(14);
			}
			fxPushNULL(parser);
			P_RESUME(15);
		case XS_TOKEN_LEFT_BRACE:
			fxParserCall(parser, K_BLOCK, f->i0);
			P_RESUME(12);
		case XS_TOKEN_LET:
			if (!f->blockIt)
				fxReportParserError(parser, parser->states[0].line, "no block");
			f->t0 = XS_TOKEN_LET;
			fxParserCallTokenFlag(parser, K_VARIABLE_STATEMENT, f->i0, f->t0, 0);
			P_RESUME(11);
		case XS_TOKEN_SWITCH:
			fxParserCall(parser, K_SWITCH, f->i0);
			P_RESUME(12);
		case XS_TOKEN_THROW:
			/* xs_no_recursion: inline fxThrowStatement across outer pc 16:
			   expression via trampoline, then node + semicolon. */
			f->i1 = parser->states[0].line;
			fxMatchToken(parser, XS_TOKEN_THROW);
			if ((!parser->states[0].crlf) && (gxTokenFlags[parser->states[0].token] & XS_TOKEN_BEGIN_EXPRESSION)) {
				fxParserCall(parser, K_COMMA, f->i1);
				P_RESUME(16);
			}
			fxReportParserError(parser, parser->states[0].line, "missing expression");
			fxPushNodeStruct(parser, 0, XS_TOKEN_UNDEFINED, f->i1);
			fxPushNodeStruct(parser, 1, XS_TOKEN_THROW, f->i1);
			fxSemicolon(parser);
			break;
		case XS_TOKEN_TRY:
			fxParserCall(parser, K_TRY, f->i0);
			P_RESUME(12);
		case XS_TOKEN_VAR:
			f->t0 = XS_TOKEN_VAR;
			fxParserCallTokenFlag(parser, K_VARIABLE_STATEMENT, f->i0, f->t0, 0);
			P_RESUME(11);
		case XS_TOKEN_WHILE:
			fxParserCall(parser, K_WHILE, f->i0);
			P_RESUME(12);
		case XS_TOKEN_WITH:
			if (parser->flags & mxStrictFlag)
				fxReportParserError(parser, parser->states[0].line, "with (strict code)");
			fxParserCall(parser, K_WITH, f->i0);
			P_RESUME(12);
#if mxExplicitResourceManagement
		case XS_TOKEN_AWAIT:
			fxLookAheadOnce(parser);
			if ((!parser->states[1].crlf) && (parser->states[1].token == XS_TOKEN_IDENTIFIER) && (parser->states[1].symbol == parser->usingSymbol) && (!parser->states[1].escaped)) {
				fxLookAheadTwice(parser);
				if ((!parser->states[2].crlf) && ((parser->states[2].token == XS_TOKEN_IDENTIFIER) || (parser->states[2].token == XS_TOKEN_AWAIT) || (parser->states[2].token == XS_TOKEN_YIELD))) {
					fxGetNextToken(parser);
					parser->states[0].token = XS_TOKEN_USING;
					if (f->blockIt <= 0)
						fxReportParserError(parser, parser->states[0].line, "no block");
					f->t0 = XS_TOKEN_USING;
					fxParserCallTokenFlag(parser, K_VARIABLE_STATEMENT, f->i0, f->t0, mxAwaitingFlag);
					P_RESUME(11);
				}
			}
			fxParserCall(parser, K_COMMA, f->i0);
			P_RESUME(9);
#endif
		case XS_TOKEN_IDENTIFIER:
			fxLookAheadOnce(parser);
			if (parser->states[1].token == XS_TOKEN_COLON) {
				fxPushSymbol(parser, parser->states[0].symbol);
				fxGetNextToken(parser);
				fxMatchToken(parser, XS_TOKEN_COLON);
				if (parser->states[0].token == XS_TOKEN_FUNCTION)
					fxReportParserError(parser, parser->states[0].line, "labeled function");
				fxCheckParserStack(parser, f->i0);
				f->blockIt = 0;
				fxParserCallParam(parser, K_STATEMENT, parser->states[0].line, 0, 0);
				P_RESUME(13);
			}
			if ((parser->states[0].symbol == parser->asyncSymbol) && (!parser->states[0].escaped)
					&& (!parser->states[1].crlf) && (parser->states[1].token == XS_TOKEN_FUNCTION)) {
				fxGetNextToken(parser);
				f->u0 = mxAsyncFlag;
				goto again;
			}
			if ((parser->states[0].symbol == parser->letSymbol) && (!parser->states[0].escaped)
					&& ((gxTokenFlags[parser->states[1].token] & XS_TOKEN_BEGIN_BINDING) || (parser->states[1].token == XS_TOKEN_AWAIT) || (parser->states[1].token == XS_TOKEN_YIELD))
					&& (f->blockIt || (!parser->states[1].crlf) || (parser->states[1].token == XS_TOKEN_LEFT_BRACKET))) {
				parser->states[0].token = XS_TOKEN_LET;
				if (!f->blockIt)
					fxReportParserError(parser, parser->states[0].line, "no block");
				f->t0 = XS_TOKEN_LET;
				fxParserCallTokenFlag(parser, K_VARIABLE_STATEMENT, f->i0, f->t0, 0);
				P_RESUME(11);
			}
#if mxExplicitResourceManagement
			if ((parser->states[0].symbol == parser->usingSymbol) && (!parser->states[0].escaped)
					&& (!parser->states[1].crlf) && ((parser->states[1].token == XS_TOKEN_IDENTIFIER) || (parser->states[1].token == XS_TOKEN_AWAIT) || (parser->states[1].token == XS_TOKEN_YIELD))) {
				parser->states[0].token = XS_TOKEN_USING;
				if (f->blockIt <= 0)
					fxReportParserError(parser, parser->states[0].line, "no block");
				f->t0 = XS_TOKEN_USING;
				fxParserCallTokenFlag(parser, K_VARIABLE_STATEMENT, f->i0, f->t0, 0);
				P_RESUME(11);
			}
#endif
			/* continue */
			mxFallThrough;
		default:
			if (gxTokenFlags[parser->states[0].token] & XS_TOKEN_BEGIN_EXPRESSION) {
				fxParserCall(parser, K_COMMA, f->i0);
				P_RESUME(9);
			}
			else {
				fxReportParserError(parser, parser->states[0].line, "invalid token %s", gxTokenNames[parser->states[0].token]);
				fxPushNULL(parser);
				fxGetNextToken(parser);
			}
			break;
		}
		break;
	case 1:
		/* resuming after function/generator expression: name in outSymbol */
		f->s0 = parser->outSymbol;
		if (f->s0) {
			txDefineNode* node = fxDefineNodeNew(parser, XS_TOKEN_DEFINE, f->s0);
			node->initializer = fxPopNode(parser);
			fxPushNode(parser, (txNode*)node);
		}
		else
			fxReportParserError(parser, parser->states[0].line, "missing identifier");
		break;
	case 9:
		fxPushNodeStruct(parser, 1, XS_TOKEN_STATEMENT, f->i0);
#ifdef XS_NR_PUMP_TRACE
		c_fprintf(stderr, "STMT9 tok=%d (%s) crlf=%d\n", (int)parser->states[0].token, gxTokenNames[parser->states[0].token], (int)parser->states[0].crlf);
#endif
		fxSemicolon(parser);
		break;
	case 10:
		/* class statement: symbol name via outSymbol */
		f->s0 = parser->outSymbol;
		if (f->s0) {
			fxPushSymbol(parser, f->s0);
			fxPushNodeStruct(parser, 1, XS_TOKEN_LET, f->i0);
			fxSwapNodes(parser);
			fxPushNodeStruct(parser, 2, XS_TOKEN_BINDING, f->i0);
		}
		else
			fxReportParserError(parser, parser->states[0].line, "missing identifier");
		break;
	case 11:
		fxSemicolon(parser);
		break;
	case 12:
		break;
	case 13:
		fxPushNodeStruct(parser, 2, XS_TOKEN_LABEL, f->i0);
		break;
	case 14:
	case 15:
		/* return statement: expression parsed (pc 14 falls through) */
		fxPushNodeStruct(parser, 1, XS_TOKEN_RETURN, f->i1);
		fxSemicolon(parser);
		break;
	case 16:
		/* throw statement: expression parsed */
		fxPushNodeStruct(parser, 1, XS_TOKEN_THROW, f->i1);
		fxSemicolon(parser);
		break;
	}
	fxParserReturn(parser);
}

void fxBlockStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->i0 = parser->states[0].line;
		fxCheckParserStack(parser, f->i0);
		fxMatchToken(parser, XS_TOKEN_LEFT_BRACE);
		fxParserCall(parser, K_STATEMENTS, f->i0);
		P_RESUME(1);
	case 1:
		fxMatchToken(parser, XS_TOKEN_RIGHT_BRACE);
		fxPushNodeStruct(parser, 1, XS_TOKEN_BLOCK, f->i0);
		break;
	}
	fxParserReturn(parser);
}

void fxBodyStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	txNode* node;
	switch (f->pc) {
	case 0:
		f->i1 = parser->nodeCount;
		f->i0 = parser->states[0].line;
		fxCheckParserStack(parser, f->i0);
		f->pc = 1;
		return;
	case 1:
		if ((parser->states[0].token == XS_TOKEN_EOF) || (parser->states[0].token == XS_TOKEN_RIGHT_BRACE)) {
			f->pc = 3;
			return;
		}
		fxParserCallParam(parser, K_STATEMENT, parser->states[0].line, 1, 0);
		P_RESUME(2);
	case 2:
		node = parser->root;
		if (node && node->description && (node->description->token == XS_TOKEN_STATEMENT)) {
			node = ((txStatementNode*)node)->expression;
			if (node && node->description && (node->description->token == XS_TOKEN_STRING)) {
				if (!(node->flags & mxStringEscapeFlag) && (c_strcmp(((txStringNode*)node)->value, "use strict") == 0)) {
					if (parser->flags & mxNotSimpleParametersFlag)
						fxReportParserError(parser, parser->states[0].line, "invalid directive");
					if (!(parser->flags & mxStrictFlag)) {
						parser->flags |= mxStrictFlag;
						if (parser->states[0].token == XS_TOKEN_IDENTIFIER)
							fxCheckStrictKeyword(parser);
					}
				}
				f->pc = 1;
				return;
			}
		}
		f->pc = 3;
		return;
	case 3:
		while ((parser->states[0].token != XS_TOKEN_EOF) && (parser->states[0].token != XS_TOKEN_RIGHT_BRACE)) {
			fxParserCallParam(parser, K_STATEMENT, parser->states[0].line, 1, 0);
			P_RESUME(4);
		}
		f->pc = 5;
		return;
	case 4:
		f->pc = 3;
		return;
	case 5:
		f->i2 = parser->nodeCount - f->i1;
		if (f->i2 > 1) {
			fxPushNodeList(parser, f->i2);
			fxPushNodeStruct(parser, 1, XS_TOKEN_STATEMENTS, f->i0);
		}
		else if (f->i2 == 0) {
			fxPushNodeStruct(parser, 0, XS_TOKEN_UNDEFINED, f->i0);
			fxPushNodeStruct(parser, 1, XS_TOKEN_STATEMENT, f->i0);
		}
		break;
	}
	fxParserReturn(parser);
}

void fxDoStatementStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->i0 = parser->states[0].line;
		fxCheckParserStack(parser, f->i0);
		fxPushNULL(parser);
		fxMatchToken(parser, XS_TOKEN_DO);
		fxParserCallParam(parser, K_STATEMENT, parser->states[0].line, 0, 0);
		P_RESUME(1);
	case 1:
		fxMatchToken(parser, XS_TOKEN_WHILE);
		fxMatchToken(parser, XS_TOKEN_LEFT_PARENTHESIS);
		fxParserCall(parser, K_COMMA, f->i0);
		P_RESUME(2);
	case 2:
		fxMatchToken(parser, XS_TOKEN_RIGHT_PARENTHESIS);
		if (parser->states[0].token == XS_TOKEN_SEMICOLON)
			fxGetNextToken(parser);
		fxPushNodeStruct(parser, 2, XS_TOKEN_DO, f->i0);
		fxPushNodeStruct(parser, 2, XS_TOKEN_LABEL, f->i0);
		break;
	}
	fxParserReturn(parser);
}

void fxWhileStatementStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->i0 = parser->states[0].line;
		fxPushNULL(parser);
		fxMatchToken(parser, XS_TOKEN_WHILE);
		fxMatchToken(parser, XS_TOKEN_LEFT_PARENTHESIS);
		fxParserCall(parser, K_COMMA, f->i0);
		P_RESUME(1);
	case 1:
		fxMatchToken(parser, XS_TOKEN_RIGHT_PARENTHESIS);
		fxParserCallParam(parser, K_STATEMENT, parser->states[0].line, 0, 0);
		P_RESUME(2);
	case 2:
		fxPushNodeStruct(parser, 2, XS_TOKEN_WHILE, f->i0);
		fxPushNodeStruct(parser, 2, XS_TOKEN_LABEL, f->i0);
		break;
	}
	fxParserReturn(parser);
}

void fxIfStatementStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->i0 = parser->states[0].line;
		fxMatchToken(parser, XS_TOKEN_IF);
		fxMatchToken(parser, XS_TOKEN_LEFT_PARENTHESIS);
		fxParserCall(parser, K_COMMA, f->i0);
		P_RESUME(1);
	case 1:
		fxMatchToken(parser, XS_TOKEN_RIGHT_PARENTHESIS);
		fxParserCallParam(parser, K_STATEMENT, parser->states[0].line, 0, 0);
		P_RESUME(2);
	case 2:
		if (parser->states[0].token == XS_TOKEN_ELSE) {
			fxMatchToken(parser, XS_TOKEN_ELSE);
			fxParserCallParam(parser, K_STATEMENT, parser->states[0].line, 0, 0);
			P_RESUME(3);
		}
		fxPushNULL(parser);
		fxPushNodeStruct(parser, 3, XS_TOKEN_IF, f->i0);
		break;
	case 3:
		fxPushNodeStruct(parser, 3, XS_TOKEN_IF, f->i0);
		break;
	}
	fxParserReturn(parser);
}

void fxWithStatementStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->i0 = parser->states[0].line;
		fxMatchToken(parser, XS_TOKEN_WITH);
		fxMatchToken(parser, XS_TOKEN_LEFT_PARENTHESIS);
		fxParserCall(parser, K_COMMA, f->i0);
		P_RESUME(1);
	case 1:
		fxMatchToken(parser, XS_TOKEN_RIGHT_PARENTHESIS);
		fxParserCallParam(parser, K_STATEMENT, parser->states[0].line, 0, 0);
		P_RESUME(2);
	case 2:
		fxPushNodeStruct(parser, 2, XS_TOKEN_WITH, f->i0);
		break;
	}
	fxParserReturn(parser);
}


void fxVariableStatementStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->t0 = f->t1; /* theToken argument (fxParserCallTokenFlag) */
		f->u0 = f->u2; /* flags argument */
		f->i0 = parser->states[0].line;
		f->i1 = 0;
		f->b0 = 0;
		fxMatchToken(parser, f->t0);
		f->pc = 1;
		return;
	case 1:
		if (!(gxTokenFlags[parser->states[0].token] & XS_TOKEN_BEGIN_BINDING)) {
			if ((f->i1 == 0) || f->b0) {
				fxPushNULL(parser);
				fxPushNULL(parser);
				fxPushNodeStruct(parser, 2, f->t0, f->i0);
				fxReportParserError(parser, parser->states[0].line, "missing identifier");
			}
			if (f->i1 > 1) {
				fxPushNodeList(parser, f->i1);
				fxPushNodeStruct(parser, 1, XS_TOKEN_STATEMENTS, f->i0);
			}
			break;
		}
		f->b0 = 0;
		fxParserCallTokenFlag(parser, K_BINDING, parser->states[0].line, f->t0, 1 | f->u0);
		P_RESUME(2);
	case 2:
		f->i1++;
		if (parser->states[0].token == XS_TOKEN_COMMA) {
			parser->flags &= ~mxForFlag;
			fxGetNextToken(parser);
			f->b0 = 1;
		}
		else {
			if ((f->i1 == 0) || f->b0) {
				fxPushNULL(parser);
				fxPushNULL(parser);
				fxPushNodeStruct(parser, 2, f->t0, f->i0);
				fxReportParserError(parser, parser->states[0].line, "missing identifier");
			}
			if (f->i1 > 1) {
				fxPushNodeList(parser, f->i1);
				fxPushNodeStruct(parser, 1, XS_TOKEN_STATEMENTS, f->i0);
			}
			break;
		}
		f->pc = 1;
		return;
	}
	fxParserReturn(parser);
}

void fxBindingStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
#ifdef XS_NR_PUMP_TRACE
	c_fprintf(stderr, "BIND f=%p pc=%d tok=%d (%s)\n", (void*)f, (int)f->pc, (int)parser->states[0].token, gxTokenNames[parser->states[0].token]);
#endif
	switch (f->pc) {
	case 0:
		f->t0 = f->t1; /* theToken argument (fxParserCallTokenFlag) */
		f->u0 = f->u2; /* flags argument */
		f->i0 = parser->states[0].line;
		fxCheckParserStack(parser, f->i0);
		if (parser->states[0].token == XS_TOKEN_IDENTIFIER) {
			fxCheckStrictSymbol(parser, parser->states[0].symbol);
			if (((f->t0 == XS_TOKEN_CONST) || (f->t0 == XS_TOKEN_LET) || (f->t0 == XS_TOKEN_USING)) && (parser->states[0].symbol == parser->letSymbol))
				fxReportParserError(parser, parser->states[0].line, "invalid identifier");
			fxPushSymbol(parser, parser->states[0].symbol);
			fxPushNodeStruct(parser, 1, f->t0, f->i0);
			if (f->u0 & mxAwaitingFlag)
				parser->root->flags |= mxAwaitingFlag;
			fxGetNextToken(parser);
		}
		else if (parser->states[0].token == XS_TOKEN_LEFT_BRACE) {
			if (f->t0 == XS_TOKEN_USING)
				fxReportParserError(parser, parser->states[0].line, "invalid using");
			fxParserCallFlag(parser, K_OBJECT_BINDING, f->i0, f->t0);
			P_RESUME(1);
		}
		else if (parser->states[0].token == XS_TOKEN_LEFT_BRACKET) {
			if (f->t0 == XS_TOKEN_USING)
				fxReportParserError(parser, parser->states[0].line, "invalid using");
			fxParserCallFlag(parser, K_ARRAY_BINDING, f->i0, f->t0);
			P_RESUME(1);
		}
		else {
			fxReportParserError(parser, parser->states[0].line, "missing identifier");
			fxPushNULL(parser);
		}
		f->pc = 2;
		return;
	case 1:
		/* xs_no_recursion: resume after a sub-binding ({...}/[...]) —
		   fall through to the initializer check (stock fxBinding always
		   continues after its if/else chain). */
	case 2:
		if ((f->u0 & 1) && (parser->states[0].token == XS_TOKEN_ASSIGN)) {
			parser->flags &= ~mxForFlag;
			fxGetNextToken(parser);
			fxParserCall(parser, K_ASSIGN, parser->states[0].line);
			P_RESUME(3);
		}
		break;
	case 3:
		fxPushNodeStruct(parser, 2, XS_TOKEN_BINDING, f->i0);
		break;
	}
	fxParserReturn(parser);
}

void fxArrayBindingStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
#ifdef XS_NR_PUMP_TRACE
	c_fprintf(stderr, "ABIND f=%p pc=%d tok=%d i1=%d\n", (void*)f, (int)f->pc, (int)parser->states[0].token, (int)f->i1);
#endif
	switch (f->pc) {
	case 0:
		f->t0 = (txToken)f->u0; /* theToken argument (fxParserCallFlag) */
		f->i1 = 0;
		f->i0 = parser->states[0].line;
		f->b0 = 1;
		fxCheckParserStack(parser, f->i0);
		fxMatchToken(parser, XS_TOKEN_LEFT_BRACKET);
		f->pc = 1;
		return;
	case 1:
		if (!((parser->states[0].token == XS_TOKEN_COMMA) || (gxTokenFlags[parser->states[0].token] & XS_TOKEN_BEGIN_BINDING))) {
			fxMatchToken(parser, XS_TOKEN_RIGHT_BRACKET);
			fxPushNodeList(parser, f->i1);
			fxPushNodeStruct(parser, 1, XS_TOKEN_ARRAY_BINDING, f->i0);
			break;
		}
		f->i2 = parser->states[0].line;
		if (parser->states[0].token == XS_TOKEN_COMMA) {
			fxGetNextToken(parser);
			if (f->b0) {
				fxPushNodeStruct(parser, 0, XS_TOKEN_SKIP_BINDING, f->i2);
				f->i1++;
			}
			else
				f->b0 = 1;
			f->pc = 1;
			return;
		}
		if (!f->b0)
			fxReportParserError(parser, parser->states[0].line, "missing ,");
		if (parser->states[0].token == XS_TOKEN_SPREAD) {
			fxParserCallTokenFlag(parser, K_REST_BINDING, f->i2, f->t0, 0);
			P_RESUME(2);
		}
		fxParserCallTokenFlag(parser, K_BINDING, f->i2, f->t0, 1);
		P_RESUME(3);
	case 2:
		f->i1++;
		/* xs_no_recursion: stock's rest-exit breaks out of the item loop
		   into the SHARED tail, which still matches the closing bracket
		   before pushing list + ARRAY_BINDING. The machine's resume case
		   must mirror that tail or the ']' is left unconsumed. */
		fxMatchToken(parser, XS_TOKEN_RIGHT_BRACKET);
		fxPushNodeList(parser, f->i1);
		fxPushNodeStruct(parser, 1, XS_TOKEN_ARRAY_BINDING, f->i0);
		break;
	case 3:
		f->i1++;
		f->b0 = 0;
		f->pc = 1;
		return;
	}
	fxParserReturn(parser);
}

void fxObjectBindingStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
#ifdef XS_NR_PUMP_TRACE
	c_fprintf(stderr, "OBIND f=%p pc=%d tok=%d i1=%d\n", (void*)f, (int)f->pc, (int)parser->states[0].token, (int)f->i1);
#endif
	switch (f->pc) {
	case 0:
		f->t0 = (txToken)f->u0; /* theToken argument (fxParserCallFlag) */
		f->i1 = 0;
		f->i0 = parser->states[0].line;
		f->u1 = 0; /* spread flag */
		f->s0 = C_NULL;
		f->t1 = XS_TOKEN_PROPERTY_BINDING;
		fxMatchToken(parser, XS_TOKEN_LEFT_BRACE);
		f->pc = 1;
		return;
	case 1:
		if (parser->states[0].token == XS_TOKEN_RIGHT_BRACE) {
			fxMatchToken(parser, XS_TOKEN_RIGHT_BRACE);
			fxPushNodeList(parser, f->i1);
			fxPushNodeStruct(parser, 1, XS_TOKEN_OBJECT_BINDING, f->i0);
			parser->root->flags |= f->u1;
			break;
		}
		f->i2 = parser->states[0].line;
		f->s0 = NULL;
		f->t1 = XS_TOKEN_PROPERTY_BINDING;
		if ((gxTokenFlags[parser->states[0].token] & XS_TOKEN_IDENTIFIER_NAME)) {
			f->s0 = parser->states[0].symbol;
			fxPushSymbol(parser, f->s0);
			f->t1 = XS_TOKEN_PROPERTY_BINDING;
		}
		else if (parser->states[0].token == XS_TOKEN_INTEGER) {
			txIndex index;
			if (fxIntegerToIndex(parser->console, parser->states[0].integer, &index)) {
				fxPushIndexNode(parser, index, f->i2);
				f->t1 = XS_TOKEN_PROPERTY_BINDING_AT;
			}
			else {
				f->s0 = fxNewParserSymbol(parser, fxIntegerToString(parser->console, parser->states[0].integer, parser->buffer, parser->bufferSize));
				fxPushSymbol(parser, f->s0);
				f->t1 = XS_TOKEN_PROPERTY_BINDING;
			}
		}
		else if (parser->states[0].token == XS_TOKEN_NUMBER) {
			txIndex index;
			if (fxNumberToIndex(parser->console, parser->states[0].number, &index)) {
				fxPushIndexNode(parser, index, f->i2);
				f->t1 = XS_TOKEN_PROPERTY_BINDING_AT;
			}
			else {
				f->s0 = fxNewParserSymbol(parser, fxNumberToString(parser->console, parser->states[0].number, parser->buffer, parser->bufferSize, 0, 0));
				fxPushSymbol(parser, f->s0);
				f->t1 = XS_TOKEN_PROPERTY_BINDING;
			}
		}
		else if (parser->states[0].token == XS_TOKEN_STRING) {
			txIndex index;
			if (fxStringToIndex(parser->console, parser->states[0].string, &index)) {
				fxPushIndexNode(parser, index, f->i2);
				f->t1 = XS_TOKEN_PROPERTY_BINDING_AT;
			}
			else {
				f->s0 = fxNewParserSymbol(parser, parser->states[0].string);
				fxPushSymbol(parser, f->s0);
				f->t1 = XS_TOKEN_PROPERTY_BINDING;
			}
		}
		else if (parser->states[0].token == XS_TOKEN_LEFT_BRACKET) {
			fxGetNextToken(parser);
			fxParserCall(parser, K_COMMA, f->i0);
			P_RESUME(4);
		}
		else if (parser->states[0].token == XS_TOKEN_SPREAD) {
			f->u1 |= mxSpreadFlag;
			fxParserCallTokenFlag(parser, K_REST_BINDING, f->i2, f->t0, 1);
			P_RESUME(5);
		}		else {
			fxReportParserError(parser, parser->states[0].line, "missing identifier");
			fxPushNULL(parser);
		}
		fxLookAheadOnce(parser);
		if (parser->states[1].token == XS_TOKEN_COLON) {
			fxGetNextToken(parser);
			fxGetNextToken(parser);
			fxParserCallTokenFlag(parser, K_BINDING, f->i2, f->t0, 1);
			P_RESUME(2);
		}
		else if (f->s0) {
			fxParserCallTokenFlag(parser, K_BINDING, f->i2, f->t0, 1);
			P_RESUME(2);
		}
		else {
			fxReportParserError(parser, parser->states[0].line, "missing :");
			fxPushNULL(parser);
			f->pc = 3;
			return;
		}
	case 2:
		fxPushNodeStruct(parser, 2, f->t1, f->i2);
		f->i1++;
		if (parser->states[0].token == XS_TOKEN_RIGHT_BRACE) {
			fxMatchToken(parser, XS_TOKEN_RIGHT_BRACE);
			fxPushNodeList(parser, f->i1);
			fxPushNodeStruct(parser, 1, XS_TOKEN_OBJECT_BINDING, f->i0);
			parser->root->flags |= f->u1;
			break;
		}
		if (parser->states[0].token == XS_TOKEN_COMMA) {
			fxGetNextToken(parser);
			f->pc = 1;
			return;
		}
		else {
			fxMatchToken(parser, XS_TOKEN_RIGHT_BRACE);
			fxPushNodeList(parser, f->i1);
			fxPushNodeStruct(parser, 1, XS_TOKEN_OBJECT_BINDING, f->i0);
			parser->root->flags |= f->u1;
		}
		break;
	case 3:
		fxPushNodeStruct(parser, 2, f->t1, f->i2);
		f->i1++;
		if (parser->states[0].token == XS_TOKEN_RIGHT_BRACE) {
			fxMatchToken(parser, XS_TOKEN_RIGHT_BRACE);
			fxPushNodeList(parser, f->i1);
			fxPushNodeStruct(parser, 1, XS_TOKEN_OBJECT_BINDING, f->i0);
			parser->root->flags |= f->u1;
			break;
		}
		if (parser->states[0].token == XS_TOKEN_COMMA) {
			fxGetNextToken(parser);
			f->pc = 1;
			return;
		}
		fxMatchToken(parser, XS_TOKEN_RIGHT_BRACE);
		fxPushNodeList(parser, f->i1);
		fxPushNodeStruct(parser, 1, XS_TOKEN_OBJECT_BINDING, f->i0);
		parser->root->flags |= f->u1;
		break;
	case 4:
		if (parser->states[0].token != XS_TOKEN_RIGHT_BRACKET)
			fxReportParserError(parser, parser->states[0].line, "missing ]");
		f->t1 = XS_TOKEN_PROPERTY_BINDING_AT;
		fxLookAheadOnce(parser);
		if (parser->states[1].token == XS_TOKEN_COLON) {
			fxGetNextToken(parser);
			fxGetNextToken(parser);
			/* xs_no_recursion: K_BINDING's machine reads its token argument
			   from f->t1 and its flags from f->u2 (CallTokenFlag layout);
			   fxParserCallFlag would deliver the token as FLAGS and leave
			   t1 stale. Stock: fxBinding(parser, theToken, 1). */
			fxParserCallTokenFlag(parser, K_BINDING, f->i2, f->t0, 1);
			P_RESUME(2);
		}
		else {
			fxReportParserError(parser, parser->states[0].line, "missing :");
			fxPushNULL(parser);
			f->pc = 3;
			return;
		}
	case 5:
		f->i1++;
		fxMatchToken(parser, XS_TOKEN_RIGHT_BRACE);
		fxPushNodeList(parser, f->i1);
		fxPushNodeStruct(parser, 1, XS_TOKEN_OBJECT_BINDING, f->i0);
		/* xs_no_recursion: stock ORs the SPREAD flag into root->flags at
		   the rest exit — that is u1 here, not u0 (the binding token). */
		parser->root->flags |= f->u1;
		break;
	}
	fxParserReturn(parser);
}

void fxRestBindingStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->t0 = f->t1; /* theToken argument (fxParserCallTokenFlag) */
		f->u0 = f->u2; /* flag argument */
		f->i0 = parser->states[0].line;
		fxMatchToken(parser, XS_TOKEN_SPREAD);
		fxParserCallTokenFlag(parser, K_BINDING, f->i0, f->t0, 0);
		P_RESUME(1);
	case 1:
		if (f->u0 && ((parser->root->description->token == XS_TOKEN_ARRAY_BINDING) || (parser->root->description->token == XS_TOKEN_OBJECT_BINDING)))
			fxReportParserError(parser, parser->states[0].line, "invalid rest");
		fxPushNodeStruct(parser, 1, XS_TOKEN_REST_BINDING, f->i0);
		break;
	}
	fxParserReturn(parser);
}

void fxParametersBindingStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->i1 = 0;
		f->i0 = parser->states[0].line;
		if (parser->states[0].token == XS_TOKEN_LEFT_PARENTHESIS) {
			fxGetNextToken(parser);
			f->pc = 1;
			return;
		}
		fxReportParserError(parser, parser->states[0].line, "missing (");
		fxPushNodeList(parser, f->i1);
		fxPushNodeStruct(parser, 1, XS_TOKEN_PARAMS_BINDING, f->i0);
		break;
	case 1:
		if (!(gxTokenFlags[parser->states[0].token] & XS_TOKEN_BEGIN_BINDING)) {
			fxMatchToken(parser, XS_TOKEN_RIGHT_PARENTHESIS);
			fxPushNodeList(parser, f->i1);
			fxPushNodeStruct(parser, 1, XS_TOKEN_PARAMS_BINDING, f->i0);
			break;
		}
		if (parser->states[0].token == XS_TOKEN_SPREAD) {
			parser->flags |= mxNotSimpleParametersFlag;
			fxParserCallTokenFlag(parser, K_REST_BINDING, f->i0, XS_TOKEN_ARG, 0);
			P_RESUME(2);
		}
		fxParserCallTokenFlag(parser, K_BINDING, f->i0, XS_TOKEN_ARG, 1);
		P_RESUME(3);
	case 2:
		f->i1++;
		fxMatchToken(parser, XS_TOKEN_RIGHT_PARENTHESIS);
		fxPushNodeList(parser, f->i1);
		fxPushNodeStruct(parser, 1, XS_TOKEN_PARAMS_BINDING, f->i0);
		break;
	case 3: {
		txBindingNode* binding = (txBindingNode*)parser->root;
		f->i1++;
		if (binding->description->token != XS_TOKEN_ARG)
			parser->flags |= mxNotSimpleParametersFlag;
		if (parser->states[0].token != XS_TOKEN_RIGHT_PARENTHESIS)
			fxMatchToken(parser, XS_TOKEN_COMMA);
		f->pc = 1;
		return;
	}
	}
	fxParserReturn(parser);
}

void fxParametersStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->i1 = 0;
		f->i0 = parser->states[0].line;
		f->b0 = 0;
		fxMatchToken(parser, XS_TOKEN_LEFT_PARENTHESIS);
		f->pc = 1;
		return;
	case 1:
		if (!((parser->states[0].token == XS_TOKEN_SPREAD) || (gxTokenFlags[parser->states[0].token] & XS_TOKEN_BEGIN_EXPRESSION))) {
			fxMatchToken(parser, XS_TOKEN_RIGHT_PARENTHESIS);
			fxPushNodeList(parser, f->i1);
			fxPushNodeStruct(parser, 1, XS_TOKEN_PARAMS, f->i0);
			if (f->b0)
				parser->root->flags |= mxSpreadFlag;
			break;
		}
		f->i2 = parser->states[0].line;
		if (parser->states[0].token == XS_TOKEN_SPREAD) {
			fxGetNextToken(parser);
			fxParserCall(parser, K_ASSIGN, f->i2);
			P_RESUME(2);
		}
		else {
			fxParserCall(parser, K_ASSIGN, f->i2);
			P_RESUME(3);
		}
	case 2:
		fxPushNodeStruct(parser, 1, XS_TOKEN_SPREAD, f->i2);
		f->b0 = 1;
		f->i1++;
		if (parser->states[0].token != XS_TOKEN_RIGHT_PARENTHESIS)
			fxMatchToken(parser, XS_TOKEN_COMMA);
		f->pc = 1;
		return;
	case 3:
		f->i1++;
		if (parser->states[0].token != XS_TOKEN_RIGHT_PARENTHESIS)
			fxMatchToken(parser, XS_TOKEN_COMMA);
		f->pc = 1;
		return;
	}
	fxParserReturn(parser);
}

void fxPropertyNameStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->i0 = parser->states[0].line;
		f->s0 = C_NULL;
		f->t0 = XS_NO_TOKEN;
		f->t1 = XS_NO_TOKEN;
		f->t2 = XS_NO_TOKEN;
		f->u0 = 0;
		fxLookAheadOnce(parser);
		f->t0 = parser->states[0].token;
		if ((gxTokenFlags[f->t0] & XS_TOKEN_IDENTIFIER_NAME)) {
			f->s0 = parser->states[0].symbol;
			if (parser->states[1].token == XS_TOKEN_COLON) {
				fxPushSymbol(parser, f->s0);
				f->t1 = XS_TOKEN_PROPERTY;
			}
			else if (fxIsKeyword(parser, parser->asyncSymbol) && (!parser->states[1].crlf)) {
				f->u0 = mxAsyncFlag;
				fxGetNextToken(parser);
				if (parser->states[0].token == XS_TOKEN_MULTIPLY) {
					f->t2 = XS_TOKEN_GENERATOR;
					fxGetNextToken(parser);
				}
				else
					f->t2 = XS_TOKEN_FUNCTION;
			}
			else if (fxIsKeyword(parser, parser->getSymbol)) {
				f->t2 = XS_TOKEN_GETTER;
				fxGetNextToken(parser);
			}
			else if (fxIsKeyword(parser, parser->setSymbol)) {
				f->t2 = XS_TOKEN_SETTER;
				fxGetNextToken(parser);
			}
			else {
				fxPushSymbol(parser, f->s0);
				f->t1 = XS_TOKEN_PROPERTY;
			}
		}
		else if (parser->states[0].token == XS_TOKEN_MULTIPLY) {
			f->t2 = XS_TOKEN_GENERATOR;
			fxGetNextToken(parser);
		}
		else if (parser->states[0].token == XS_TOKEN_PRIVATE_IDENTIFIER) {
			f->s0 = parser->states[0].symbol;
			fxPushSymbol(parser, f->s0);
			f->t1 = XS_TOKEN_PRIVATE_PROPERTY;
		}
		else if (parser->states[0].token == XS_TOKEN_INTEGER) {
			txIndex index;
			if (fxIntegerToIndex(parser->console, parser->states[0].integer, &index)) {
				fxPushIndexNode(parser, index, f->i0);
				f->t1 = XS_TOKEN_PROPERTY_AT;
			}
			else {
				f->s0 = fxNewParserSymbol(parser, fxIntegerToString(parser->console, parser->states[0].integer, parser->buffer, parser->bufferSize));
				fxPushSymbol(parser, f->s0);
				f->t1 = XS_TOKEN_PROPERTY;
			}
		}
		else if (parser->states[0].token == XS_TOKEN_NUMBER) {
			txIndex index;
			if (fxNumberToIndex(parser->console, parser->states[0].number, &index)) {
				fxPushIndexNode(parser, index, f->i0);
				f->t1 = XS_TOKEN_PROPERTY_AT;
			}
			else {
				f->s0 = fxNewParserSymbol(parser, fxNumberToString(parser->console, parser->states[0].number, parser->buffer, parser->bufferSize, 0, 0));
				fxPushSymbol(parser, f->s0);
				f->t1 = XS_TOKEN_PROPERTY;
			}
		}
		else if (parser->states[0].token == XS_TOKEN_STRING) {
			txIndex index;
			if (fxStringToIndex(parser->console, parser->states[0].string, &index)) {
				fxPushIndexNode(parser, index, f->i0);
				f->t1 = XS_TOKEN_PROPERTY_AT;
			}
			else {
				f->s0 = fxNewParserSymbol(parser, parser->states[0].string);
				fxPushSymbol(parser, f->s0);
				f->t1 = XS_TOKEN_PROPERTY;
			}
		}
		else if (parser->states[0].token == XS_TOKEN_LEFT_BRACKET) {
			fxGetNextToken(parser);
			fxParserCall(parser, K_COMMA, f->i0);
			P_RESUME(1);
		}
		else {
			fxReportParserError(parser, parser->states[0].line, "missing identifier");
			fxPushNULL(parser);
		}
		if (f->t2 != XS_NO_TOKEN) {
			f->pc = 2;
			return;
		}
		else
			fxGetNextToken(parser);
		f->pc = 4;
		return;
	case 1:
		if (parser->states[0].token != XS_TOKEN_RIGHT_BRACKET)
			fxReportParserError(parser, parser->states[0].line, "missing ]");
		f->t1 = XS_TOKEN_PROPERTY_AT;
		fxGetNextToken(parser);
		if (f->t2 != XS_NO_TOKEN) {
			f->pc = 2;
			return;
		}
		f->pc = 4;
		return;
	case 2:
		if ((gxTokenFlags[parser->states[0].token] & XS_TOKEN_IDENTIFIER_NAME)) {
			f->s0 = parser->states[0].symbol;
			fxPushSymbol(parser, f->s0);
			f->t1 = XS_TOKEN_PROPERTY;
			fxGetNextToken(parser);
		}
		else if (parser->states[0].token == XS_TOKEN_PRIVATE_IDENTIFIER) {
			f->s0 = parser->states[0].symbol;
			fxPushSymbol(parser, f->s0);
			f->t1 = XS_TOKEN_PRIVATE_PROPERTY;
			fxGetNextToken(parser);
		}
		else if (parser->states[0].token == XS_TOKEN_INTEGER) {
			txIndex index;
			if (fxIntegerToIndex(parser->console, parser->states[0].integer, &index)) {
				fxPushIndexNode(parser, index, f->i0);
				f->t1 = XS_TOKEN_PROPERTY_AT;
			}
			else {
				f->s0 = fxNewParserSymbol(parser, fxIntegerToString(parser->console, parser->states[0].integer, parser->buffer, parser->bufferSize));
				fxPushSymbol(parser, f->s0);
				f->t1 = XS_TOKEN_PROPERTY;
			}
			fxGetNextToken(parser);
		}
		else if (parser->states[0].token == XS_TOKEN_NUMBER) {
			txIndex index;
			if (fxNumberToIndex(parser->console, parser->states[0].number, &index)) {
				fxPushIndexNode(parser, index, f->i0);
				f->t1 = XS_TOKEN_PROPERTY_AT;
			}
			else {
				f->s0 = fxNewParserSymbol(parser, fxNumberToString(parser->console, parser->states[0].number, parser->buffer, parser->bufferSize, 0, 0));
				fxPushSymbol(parser, f->s0);
				f->t1 = XS_TOKEN_PROPERTY;
			}
			fxGetNextToken(parser);
		}
		else if (parser->states[0].token == XS_TOKEN_STRING) {
			txIndex index;
			if (fxStringToIndex(parser->console, parser->states[0].string, &index)) {
				fxPushIndexNode(parser, index, f->i0);
				f->t1 = XS_TOKEN_PROPERTY_AT;
			}
			else {
				f->s0 = fxNewParserSymbol(parser, parser->states[0].string);
				fxPushSymbol(parser, f->s0);
				f->t1 = XS_TOKEN_PROPERTY;
			}
			fxGetNextToken(parser);
		}
		else if (parser->states[0].token == XS_TOKEN_LEFT_BRACKET) {
			fxGetNextToken(parser);
			fxParserCall(parser, K_COMMA, f->i0);
			P_RESUME(3);
		}
		else if (f->t2 == XS_TOKEN_GETTER) {
			fxPushSymbol(parser, f->s0);
			f->t1 = XS_TOKEN_PROPERTY;
			f->t2 = XS_NO_TOKEN;
		}
		else if (f->t2 == XS_TOKEN_SETTER) {
			fxPushSymbol(parser, f->s0);
			f->t1 = XS_TOKEN_PROPERTY;
			f->t2 = XS_NO_TOKEN;
		}
		else {
			fxReportParserError(parser, parser->states[0].line, "missing identifier");
			fxPushNULL(parser);
		}
		f->pc = 4;
		return;
	case 3:
		if (parser->states[0].token != XS_TOKEN_RIGHT_BRACKET)
			fxReportParserError(parser, parser->states[0].line, "missing ]");
		f->t1 = XS_TOKEN_PROPERTY_AT;
		fxGetNextToken(parser);
		f->pc = 4;
		return;
	case 4:
		parser->outSymbol = f->s0;
		parser->outToken0 = f->t0;
		parser->outToken1 = f->t1;
		parser->outToken2 = f->t2;
		parser->outFlags = f->u0;
		break;
	}
	fxParserReturn(parser);
}

void fxTemplateExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->i1 = 0;
		f->i0 = parser->states[0].line;
		fxPushStringNode(parser, parser->states[0].stringLength, parser->states[0].string, f->i0);
		fxPushRawNode(parser, parser->states[0].rawLength, parser->states[0].raw, f->i0);
		fxPushNodeStruct(parser, 2, XS_TOKEN_TEMPLATE_MIDDLE, f->i0);
		f->i1++;
		f->pc = 1;
		return;
	case 1:
		fxGetNextToken(parser);
		if (parser->states[0].token != XS_TOKEN_RIGHT_BRACE) {
			fxParserCall(parser, K_COMMA, f->i0);
			P_RESUME(2);
		}
		f->pc = 3;
		return;
	case 2:
		f->i1++;
		f->pc = 3;
		return;
	case 3:
		if (parser->states[0].token != XS_TOKEN_RIGHT_BRACE)
			fxReportParserError(parser, parser->states[0].line, "missing }");
		fxGetNextTokenTemplate(parser);
		fxPushStringNode(parser, parser->states[0].stringLength, parser->states[0].string, f->i0);
		fxPushRawNode(parser, parser->states[0].rawLength, parser->states[0].raw, f->i0);
		fxPushNodeStruct(parser, 2, XS_TOKEN_TEMPLATE_MIDDLE, f->i0);
		f->i1++;
		if (parser->states[0].token == XS_TOKEN_TEMPLATE_TAIL) {
			fxGetNextToken(parser);
			fxPushNodeList(parser, f->i1);
			break;
		}
		f->pc = 1;
		return;
	}
	fxParserReturn(parser);
}

void fxYieldExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->i0 = parser->states[0].line;
		if (!(parser->flags & mxYieldFlag))
			fxReportParserError(parser, parser->states[0].line, "invalid yield");
		else
			parser->flags |= mxYieldingFlag;
		fxMatchToken(parser, XS_TOKEN_YIELD);
		if ((!parser->states[0].crlf) && (parser->states[0].token == XS_TOKEN_MULTIPLY)) {
			fxGetNextToken(parser);
			fxParserCall(parser, K_ASSIGN, f->i0);
			P_RESUME(1);
		}
		if ((!parser->states[0].crlf) && (gxTokenFlags[parser->states[0].token] & XS_TOKEN_BEGIN_EXPRESSION)) {
			fxParserCall(parser, K_ASSIGN, f->i0);
			P_RESUME(2);
		}
		fxPushNodeStruct(parser, 0, XS_TOKEN_UNDEFINED, f->i0);
		fxPushNodeStruct(parser, 1, XS_TOKEN_YIELD, f->i0);
		break;
	case 1:
		fxPushNodeStruct(parser, 1, XS_TOKEN_DELEGATE, f->i0);
		break;
	case 2:
		fxPushNodeStruct(parser, 1, XS_TOKEN_YIELD, f->i0);
		break;
	}
	fxParserReturn(parser);
}

void fxArrayExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->i1 = 0;
		f->b0 = 1;
		f->i0 = parser->states[0].line;
		f->b1 = 0;
		fxMatchToken(parser, XS_TOKEN_LEFT_BRACKET);
		f->pc = 1;
		return;
	case 1:
		if (!((parser->states[0].token == XS_TOKEN_COMMA) || (parser->states[0].token == XS_TOKEN_SPREAD) || (gxTokenFlags[parser->states[0].token] & XS_TOKEN_BEGIN_EXPRESSION))) {
			fxMatchToken(parser, XS_TOKEN_RIGHT_BRACKET);
			fxPushNodeList(parser, f->i1);
			fxPushNodeStruct(parser, 1, XS_TOKEN_ARRAY, f->i0);
			if (f->i1 && f->b0)
				parser->root->flags |= mxElisionFlag;
			if (f->b1)
				parser->root->flags |= mxSpreadFlag;
			break;
		}
		f->i2 = parser->states[0].line;
		if (parser->states[0].token == XS_TOKEN_COMMA) {
			fxGetNextToken(parser);
			if (f->b0) {
				fxPushNodeStruct(parser, 0, XS_TOKEN_ELISION, f->i2);
				f->i1++;
			}
			else
				f->b0 = 1;
			f->pc = 1;
			return;
		}
		else if (parser->states[0].token == XS_TOKEN_SPREAD) {
			fxGetNextToken(parser);
			if (!f->b0)
				fxReportParserError(parser, parser->states[0].line, "missing ,");
			fxParserCall(parser, K_ASSIGN, f->i2);
			P_RESUME(2);
		}
		else {
			if (!f->b0)
				fxReportParserError(parser, parser->states[0].line, "missing ,");
			fxParserCall(parser, K_ASSIGN, f->i2);
			P_RESUME(3);
		}
	case 2:
		fxPushNodeStruct(parser, 1, XS_TOKEN_SPREAD, f->i2);
		f->i1++;
		f->b0 = 0;
		f->b1 = 1;
		f->pc = 1;
		return;
	case 3:
		f->i1++;
		f->b0 = 0;
		f->pc = 1;
		return;
	}
	fxParserReturn(parser);
}


void fxParametersBindingFromStep(txParser* parser)
{
	/* The FromExpression family (fxBindingFromExpression and friends)
	   transforms already-parsed trees; stock runs it recursively over the
	   pattern depth. Here it runs native but with an explicit stack probe
	   so an abusive nesting aborts (SyntaxError path) instead of smashing
	   the game-task stack. */
	txParserFrame* f = P_FRAME;
	fxCheckParserStack(parser, parser->states[0].line);
	f->n0 = fxParametersBindingFromExpressions(parser, parser->root);
	f->u0 = f->n0 ? 1 : 0;
	fxParserReturn(parser);
}

/* xs_no_recursion: the binding family and the JSON module rules keep
   their stock signatures (they are also called from still-native leaf
   code such as the export declaration path) but now forward to the
   trampoline instead of recursing natively. */









void fxForStatementStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->i0 = parser->states[0].line;
		f->i1 = 0; /* awaitFlag */
		f->i2 = 0; /* expressionFlag */
		f->t0 = XS_NO_TOKEN;
		fxPushNULL(parser);
		fxMatchToken(parser, XS_TOKEN_FOR);
		if (parser->states[0].token == XS_TOKEN_AWAIT) {
			f->i1 = 1;
			fxMatchToken(parser, XS_TOKEN_AWAIT);
		}
		fxMatchToken(parser, XS_TOKEN_LEFT_PARENTHESIS);
		fxLookAheadOnce(parser);
		parser->flags |= mxForFlag;
		if (parser->states[0].token == XS_TOKEN_SEMICOLON) {
			fxPushNULL(parser);
			f->pc = 2;
			return;
		}
		if (parser->states[0].token == XS_TOKEN_CONST) {
			fxParserCallTokenFlag(parser, K_VARIABLE_STATEMENT, f->i0, XS_TOKEN_CONST, 0);
			P_RESUME(1);
		}
		if (parser->states[0].token == XS_TOKEN_LET) {
			fxParserCallTokenFlag(parser, K_VARIABLE_STATEMENT, f->i0, XS_TOKEN_LET, 0);
			P_RESUME(1);
		}
		if (fxIsKeyword(parser, parser->letSymbol) && (gxTokenFlags[parser->states[1].token] & XS_TOKEN_BEGIN_BINDING)) {
			parser->states[0].token = XS_TOKEN_LET;
			fxParserCallTokenFlag(parser, K_VARIABLE_STATEMENT, f->i0, XS_TOKEN_LET, 0);
			P_RESUME(1);
		}
#if mxExplicitResourceManagement
		if ((parser->states[0].token == XS_TOKEN_IDENTIFIER) && (parser->states[0].symbol == parser->usingSymbol) && (!parser->states[0].escaped)
				&& (!parser->states[1].crlf) && ((parser->states[1].token == XS_TOKEN_IDENTIFIER) || (parser->states[1].token == XS_TOKEN_AWAIT) || (parser->states[1].token == XS_TOKEN_YIELD))) {
			fxLookAheadTwice(parser);
			if ((parser->states[1].symbol == parser->ofSymbol) && (!parser->states[1].escaped) && (parser->states[2].token != XS_TOKEN_ASSIGN)) {
				f->pc = 10;
				return;
			}
			parser->states[0].token = XS_TOKEN_USING;
			fxParserCallTokenFlag(parser, K_VARIABLE_STATEMENT, f->i0, XS_TOKEN_USING, 0);
			P_RESUME(1);
		}
		if ((parser->states[0].token == XS_TOKEN_AWAIT)
				&& (!parser->states[1].crlf) && (parser->states[1].token == XS_TOKEN_IDENTIFIER) && (parser->states[1].symbol == parser->usingSymbol) && (!parser->states[1].escaped)) {
			fxLookAheadTwice(parser);
			if ((!parser->states[2].crlf) && ((parser->states[2].token == XS_TOKEN_IDENTIFIER) || (parser->states[2].token == XS_TOKEN_AWAIT) || (parser->states[2].token == XS_TOKEN_YIELD))) {
				fxGetNextToken(parser);
				parser->states[0].token = XS_TOKEN_USING;
				fxParserCallTokenFlag(parser, K_VARIABLE_STATEMENT, f->i0, XS_TOKEN_USING, mxAwaitingFlag);
				P_RESUME(1);
			}
			f->pc = 10;
			return;
		}
#endif
		if (parser->states[0].token == XS_TOKEN_VAR) {
			fxParserCallTokenFlag(parser, K_VARIABLE_STATEMENT, f->i0, XS_TOKEN_VAR, 0);
			P_RESUME(1);
		}
		f->pc = 10;
		return;
	case 10:
		parser->flags |= mxForFlag;
		fxParserCall(parser, K_COMMA, f->i0);
		P_RESUME(11);
	case 11:
		f->i2 = 1;
		f->pc = 2;
		return;
	case 1:
		f->pc = 2;
		return;
	case 2:
		parser->flags &= ~mxForFlag;
		if (f->i1 && !fxIsKeyword(parser, parser->ofSymbol))
			fxReportParserError(parser, parser->states[0].line, "invalid for await");
		if (fxIsToken(parser, XS_TOKEN_IN) || fxIsKeyword(parser, parser->ofSymbol)) {
			if (f->i2) {
				if (!fxCheckReference(parser, XS_TOKEN_ASSIGN))
					fxReportParserError(parser, parser->states[0].line, "no reference");
			}
			else {
				f->t1 = parser->root->description->token;
				if (f->t1 == XS_TOKEN_BINDING) {
					if (((txBindingNode*)(parser->root))->initializer)
						fxReportParserError(parser, parser->states[0].line, "invalid binding initializer");
				}
				if (fxIsToken(parser, XS_TOKEN_IN) && (f->t1 == XS_TOKEN_USING))
					fxReportParserError(parser, parser->states[0].line, "invalid using in");
			}
			f->t0 = parser->states[0].token;
			fxGetNextToken(parser);
			if (f->t0 == XS_TOKEN_IN)
				fxParserCall(parser, K_COMMA, f->i0);
			else
				fxParserCall(parser, K_ASSIGN, f->i0);
			P_RESUME(3);
		}
		f->pc = 5;
		return;
	case 3:
		fxMatchToken(parser, XS_TOKEN_RIGHT_PARENTHESIS);
		fxParserCallParam(parser, K_STATEMENT, parser->states[0].line, 0, 0);
		P_RESUME(4);
	case 4:
		if (f->i1) {
			fxPushNodeStruct(parser, 3, XS_TOKEN_FOR_AWAIT_OF, f->i0);
			parser->flags |= mxAwaitingFlag;
		}
		else if (f->t0 == XS_TOKEN_IN)
			fxPushNodeStruct(parser, 3, XS_TOKEN_FOR_IN, f->i0);
		else
			fxPushNodeStruct(parser, 3, XS_TOKEN_FOR_OF, f->i0);
		fxPushNodeStruct(parser, 2, XS_TOKEN_LABEL, f->i0);
		break;
	case 5:
		if (f->i2)
			fxPushNodeStruct(parser, 1, XS_TOKEN_STATEMENT, f->i0);
		fxMatchToken(parser, XS_TOKEN_SEMICOLON);
		if (gxTokenFlags[parser->states[0].token] & XS_TOKEN_BEGIN_EXPRESSION)
			fxParserCall(parser, K_COMMA, f->i0);
		else
			fxPushNULL(parser);
		P_RESUME(6);
	case 6:
		fxMatchToken(parser, XS_TOKEN_SEMICOLON);
		if (gxTokenFlags[parser->states[0].token] & XS_TOKEN_BEGIN_EXPRESSION)
			fxParserCall(parser, K_COMMA, f->i0);
		else
			fxPushNULL(parser);
		P_RESUME(7);
	case 7:
		fxMatchToken(parser, XS_TOKEN_RIGHT_PARENTHESIS);
		fxCheckParserStack(parser, f->i0);
		fxParserCallParam(parser, K_STATEMENT, parser->states[0].line, 0, 0);
		P_RESUME(8);
	case 8:
		fxPushNodeStruct(parser, 4, XS_TOKEN_FOR, f->i0);
		fxPushNodeStruct(parser, 2, XS_TOKEN_LABEL, f->i0);
		break;
	}
	fxParserReturn(parser);
}

void fxSwitchStatementStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->i3 = 0; /* aCount */
		f->b1 = 0; /* defaultFlag */
		f->i0 = parser->states[0].line;
		fxMatchToken(parser, XS_TOKEN_SWITCH);
		fxMatchToken(parser, XS_TOKEN_LEFT_PARENTHESIS);
		fxParserCall(parser, K_COMMA, f->i0);
		P_RESUME(1);
	case 1:
		fxMatchToken(parser, XS_TOKEN_RIGHT_PARENTHESIS);
		fxMatchToken(parser, XS_TOKEN_LEFT_BRACE);
		f->pc = 2;
		return;
	case 2:
		if (!((parser->states[0].token == XS_TOKEN_CASE) || (parser->states[0].token == XS_TOKEN_DEFAULT))) {
			fxMatchToken(parser, XS_TOKEN_RIGHT_BRACE);
			fxPushNodeList(parser, f->i3);
			fxPushNodeStruct(parser, 2, XS_TOKEN_SWITCH, f->i0);
			break;
		}
		f->i2 = parser->states[0].line; /* case line */
		f->i1 = 0; /* case count */
		if (parser->states[0].token == XS_TOKEN_CASE) {
			fxMatchToken(parser, XS_TOKEN_CASE);
			fxParserCall(parser, K_COMMA, f->i2);
			P_RESUME(3);
		}
		fxMatchToken(parser, XS_TOKEN_DEFAULT);
		if (f->b1)
			fxReportParserError(parser, parser->states[0].line, "invalid default");
		fxPushNULL(parser);
		fxMatchToken(parser, XS_TOKEN_COLON);
		f->b0 = 1; /* default case */
		f->pc = 4;
		return;
	case 3:
		fxMatchToken(parser, XS_TOKEN_COLON);
		f->i1 = parser->nodeCount;
		f->b0 = 0;
		f->pc = 4;
		return;
	case 4:
		if (f->b0)
			f->i1 = parser->nodeCount;
		f->pc = 5;
		return;
	case 5:
		if (!(gxTokenFlags[parser->states[0].token] & XS_TOKEN_BEGIN_STATEMENT)) {
			f->i1 = parser->nodeCount - f->i1;
			if (f->i1 > 1) {
				fxPushNodeList(parser, f->i1);
				fxPushNodeStruct(parser, 1, XS_TOKEN_STATEMENTS, f->i2);
			}
			else if (f->i1 == 0)
				fxPushNULL(parser);
			fxPushNodeStruct(parser, 2, XS_TOKEN_CASE, f->i2);
			f->i3++;
			f->pc = 2;
			return;
		}
		fxParserCallParam(parser, K_STATEMENT, parser->states[0].line, -1, 0);
		P_RESUME(5);
	}
	fxParserReturn(parser);
}

void fxTryStatementStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->i0 = parser->states[0].line;
		f->b0 = 0;
		fxMatchToken(parser, XS_TOKEN_TRY);
		fxParserCall(parser, K_BLOCK, f->i0);
		P_RESUME(1);
	case 1:
		if (parser->states[0].token != XS_TOKEN_CATCH) {
			fxPushNULL(parser);
			f->pc = 3;
			return;
		}
		f->i1 = parser->states[0].line;
		fxMatchToken(parser, XS_TOKEN_CATCH);
		if (parser->states[0].token == XS_TOKEN_LEFT_PARENTHESIS) {
			fxMatchToken(parser, XS_TOKEN_LEFT_PARENTHESIS);
			fxParserCallTokenFlag(parser, K_BINDING, f->i1, XS_TOKEN_LET, 1);
			P_RESUME(2);
		}
		fxPushNULL(parser);
		f->pc = 20;
		return;
	case 2:
		fxMatchToken(parser, XS_TOKEN_RIGHT_PARENTHESIS);
		f->pc = 20;
		return;
	case 20:
		/* stock parses the catch body as a bare `{ STATEMENTS }` (no BLOCK
		   node): fxMatchToken('{'); fxStatements(); fxMatchToken('}'). */
		fxMatchToken(parser, XS_TOKEN_LEFT_BRACE);
		fxParserCall(parser, K_STATEMENTS, f->i0);
		P_RESUME(22);
		return;
	case 22:
		fxMatchToken(parser, XS_TOKEN_RIGHT_BRACE);
		f->pc = 21;
		return;
	case 21:
		fxPushNodeStruct(parser, 2, XS_TOKEN_CATCH, f->i1);
		f->b0 = 1;
		f->pc = 3;
		return;
	case 3:
		if (parser->states[0].token != XS_TOKEN_FINALLY) {
			fxPushNULL(parser);
			if (!f->b0)
				fxReportParserError(parser, parser->states[0].line, "missing catch or finally");
			fxPushNodeStruct(parser, 3, XS_TOKEN_TRY, f->i0);
			break;
		}
		fxMatchToken(parser, XS_TOKEN_FINALLY);
		fxParserCall(parser, K_BLOCK, f->i0);
		P_RESUME(4);
	case 4:
		f->b0 = 1; /* finally counts like stock's ok = 1 */
		fxPushNodeStruct(parser, 3, XS_TOKEN_TRY, f->i0);
		break;
	}
	fxParserReturn(parser);
}

void fxArrowExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->u1 = f->u2; /* the flag argument */
		f->i0 = f->line;
		f->u0 = parser->flags;
		parser->flags &= ~(mxAsyncFlag | mxGeneratorFlag);
		parser->flags |= mxArrowFlag | f->u1;
		fxMatchToken(parser, XS_TOKEN_ARROW);
		fxPushNULL(parser);
		fxSwapNodes(parser);
		if (parser->states[0].token == XS_TOKEN_LEFT_BRACE) {
			fxMatchToken(parser, XS_TOKEN_LEFT_BRACE);
			fxParserCall(parser, K_BODY, f->i0);
			P_RESUME(1);
		}
		fxParserCall(parser, K_ASSIGN, f->i0);
		P_RESUME(2);
	case 1:
		fxPushNodeStruct(parser, 1, XS_TOKEN_BODY, f->i0);
		fxMatchToken(parser, XS_TOKEN_RIGHT_BRACE);
		f->pc = 3;
		return;
	case 2:
		fxPushNodeStruct(parser, 1, XS_TOKEN_RETURN, f->i0);
		fxPushNodeStruct(parser, 1, XS_TOKEN_BODY, f->i0);
		if (!(f->u0 & mxAsyncFlag) && (f->u1 & mxAsyncFlag)) {
			if (parser->states[0].token == XS_TOKEN_AWAIT)
				parser->states[0].token = XS_TOKEN_IDENTIFIER;
		}
		f->pc = 3;
		return;
	case 3:
		fxPushNodeStruct(parser, 3, XS_TOKEN_FUNCTION, f->i0);
		parser->root->flags = parser->flags & (mxStrictFlag | mxFieldFlag | mxNotSimpleParametersFlag | mxArrowFlag | mxSuperFlag | f->u1);
		if (!(f->u0 & mxStrictFlag) && (parser->flags & mxStrictFlag))
			fxCheckStrictFunction(parser, (txFunctionNode*)parser->root);
		parser->flags = f->u0 | (parser->flags & (mxArgumentsFlag | mxEvalFlag));
		break;
	}
	fxParserReturn(parser);
}

void fxFunctionExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->u1 = f->u2; /* flag */
		f->i0 = f->line; /* theLine */
		f->u0 = parser->flags;
		parser->flags = (f->u0 & (mxParserFlags | mxStrictFlag)) | mxFunctionFlag | mxTargetFlag | f->u1;
		if ((parser->states[0].token == XS_TOKEN_IDENTIFIER)
				|| ((f->u0 & mxGeneratorFlag) && !(f->u0 & mxStrictFlag) && (parser->states[0].token == XS_TOKEN_YIELD))
				|| (parser->states[0].token == XS_TOKEN_AWAIT)) {
			f->s0 = parser->states[0].symbol;
			parser->outSymbol = f->s0;
			fxPushSymbol(parser, f->s0);
			fxCheckStrictSymbol(parser, f->s0);
			fxGetNextToken(parser);
		}
		else {
			f->s0 = NULL;
			parser->outSymbol = NULL;
			fxPushNULL(parser);
		}
		fxParserCall(parser, K_PARAMETERS_BINDING, f->i0);
		P_RESUME(1);
	case 1:
		if (parser->states[0].token == XS_TOKEN_HOST) {
			fxGetNextToken(parser);
			if (parser->states[0].token == XS_TOKEN_STRING) {
				fxPushStringNode(parser, parser->states[0].stringLength, parser->states[0].string, f->i0);
				fxGetNextToken(parser);
			}
			else {
				fxReportParserError(parser, parser->states[0].line, "invalid host function");
				fxPushNULL(parser);
			}
			fxPushNodeStruct(parser, 3, XS_TOKEN_HOST, f->i0);
			parser->root->flags = parser->flags & (mxStrictFlag | mxNotSimpleParametersFlag | mxTargetFlag | mxArgumentsFlag | mxEvalFlag | f->u1);
			if (!(f->u0 & mxStrictFlag) && (parser->flags & mxStrictFlag))
				fxCheckStrictFunction(parser, (txFunctionNode*)parser->root);
			parser->flags = f->u0;
			break;
		}
		fxMatchToken(parser, XS_TOKEN_LEFT_BRACE);
		fxParserCall(parser, K_BODY, f->i0);
		P_RESUME(2);
	case 2:
		fxPushNodeStruct(parser, 1, XS_TOKEN_BODY, f->i0);
		fxPushNodeStruct(parser, 3, XS_TOKEN_FUNCTION, f->i0);
		parser->root->flags = parser->flags & (mxStrictFlag | mxNotSimpleParametersFlag | mxTargetFlag | mxArgumentsFlag | mxEvalFlag | f->u1);
		if (!(f->u0 & mxStrictFlag) && (parser->flags & mxStrictFlag))
			fxCheckStrictFunction(parser, (txFunctionNode*)parser->root);
		if (parser->flags & mxNativeFlag)
			fxCheckNativeFunction(parser);
		parser->flags = f->u0;
		fxMatchToken(parser, XS_TOKEN_RIGHT_BRACE);
		break;
	}
	fxParserReturn(parser);
}

void fxGeneratorExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->u1 = f->u2; /* flag */
		f->i0 = f->line; /* theLine */
		f->u0 = parser->flags;
		parser->flags = (f->u0 & (mxParserFlags | mxStrictFlag)) | mxGeneratorFlag | mxTargetFlag | f->u1;
		if ((parser->states[0].token == XS_TOKEN_IDENTIFIER)
				|| (parser->states[0].token == XS_TOKEN_AWAIT)) {
			f->s0 = parser->states[0].symbol;
			parser->outSymbol = f->s0;
			fxPushSymbol(parser, f->s0);
			if (f->s0 == parser->yieldSymbol)
				fxReportParserError(parser, parser->states[0].line, "invalid yield");
			else if ((parser->flags & mxAsyncFlag) && (f->s0 == parser->awaitSymbol))
				fxReportParserError(parser, parser->states[0].line, "invalid await");
			fxCheckStrictSymbol(parser, f->s0);
			fxGetNextToken(parser);
		}
		else {
			f->s0 = NULL;
			parser->outSymbol = NULL;
			fxPushNULL(parser);
		}
		fxParserCall(parser, K_PARAMETERS_BINDING, f->i0);
		P_RESUME(1);
	case 1:
		fxMatchToken(parser, XS_TOKEN_LEFT_BRACE);
		parser->flags |= mxYieldFlag;
		fxParserCall(parser, K_BODY, f->i0);
		P_RESUME(2);
	case 2:
		parser->flags &= ~mxYieldFlag;
		fxPushNodeStruct(parser, 1, XS_TOKEN_BODY, f->i0);
		fxPushNodeStruct(parser, 3, XS_TOKEN_GENERATOR, f->i0);
		parser->root->flags = parser->flags & (mxStrictFlag | mxNotSimpleParametersFlag | mxGeneratorFlag | mxArgumentsFlag | mxEvalFlag | f->u1);
		if (!(f->u0 & mxStrictFlag) && (parser->flags & mxStrictFlag))
			fxCheckStrictFunction(parser, (txFunctionNode*)parser->root);
		parser->flags = f->u0;
		fxMatchToken(parser, XS_TOKEN_RIGHT_BRACE);
		break;
	}
	fxParserReturn(parser);
}

void fxObjectExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->i1 = 0; /* count */
		f->i0 = parser->states[0].line;
		f->n0 = parser->root; /* base for uniqueness check */
		fxMatchToken(parser, XS_TOKEN_LEFT_BRACE);
		f->pc = 1;
		return;
	case 1:
		if (parser->states[0].token == XS_TOKEN_RIGHT_BRACE) {
			fxMatchToken(parser, XS_TOKEN_RIGHT_BRACE);
			fxPushNodeList(parser, f->i1);
			fxPushNodeStruct(parser, 1, XS_TOKEN_OBJECT, f->i0);
			break;
		}
		f->i2 = parser->states[0].line;
		f->u1 = 0;
		if (parser->states[0].token == XS_TOKEN_SPREAD) {
			fxGetNextToken(parser);
			fxParserCall(parser, K_ASSIGN, f->i2);
			P_RESUME(2);
		}
		fxParserCall(parser, K_PROPERTY_NAME, f->i2);
		P_RESUME(3);
	case 2:
		fxPushNodeStruct(parser, 1, XS_TOKEN_SPREAD, f->i2);
		if (parser->states[0].token == XS_TOKEN_COMMA)
			parser->root->flags |= mxElisionFlag;
		f->t1 = XS_NO_TOKEN; /* spread already pushed its node */
		f->i1++;
		if (parser->states[0].token == XS_TOKEN_RIGHT_BRACE) {
			f->pc = 1;
			return;
		}
		fxMatchToken(parser, XS_TOKEN_COMMA);
		f->pc = 1;
		return;
	case 3:
		f->s0 = parser->outSymbol;
		f->t0 = parser->outToken0;
		f->t1 = parser->outToken1;
		f->t2 = parser->outToken2;
		f->u1 = parser->outFlags;
		if (f->t1 == XS_TOKEN_PRIVATE_PROPERTY) {
			fxReportParserError(parser, parser->states[0].line, "invalid private property");
			f->pc = 8;
			return;
		}
		if ((f->t2 == XS_TOKEN_GETTER) || (f->t2 == XS_TOKEN_SETTER)) {
			f->u1 |= mxShorthandFlag | ((f->t2 == XS_TOKEN_GETTER) ? mxGetterFlag : mxSetterFlag);
			if (parser->states[0].token == XS_TOKEN_LEFT_PARENTHESIS)
				fxParserCallTokenFlag(parser, K_FUNCTION_EXPR, f->i2, XS_NO_TOKEN, mxSuperFlag);
			else
				fxReportParserError(parser, parser->states[0].line, "missing (");
			f->pc = 8;
			return;
		}
		if (f->t2 == XS_TOKEN_GENERATOR) {
			f->u1 |= mxShorthandFlag | mxMethodFlag;
			if (parser->states[0].token == XS_TOKEN_LEFT_PARENTHESIS)
				fxParserCallTokenFlag(parser, K_GENERATOR_EXPR, f->i2, XS_NO_TOKEN, mxSuperFlag | f->u1);
			else
				fxReportParserError(parser, parser->states[0].line, "missing (");
			f->pc = 8;
			return;
		}
		if (f->t2 == XS_TOKEN_FUNCTION) {
			f->u1 |= mxShorthandFlag | mxMethodFlag;
			if (parser->states[0].token == XS_TOKEN_LEFT_PARENTHESIS)
				fxParserCallTokenFlag(parser, K_FUNCTION_EXPR, f->i2, XS_NO_TOKEN, mxSuperFlag | f->u1);
			else
				fxReportParserError(parser, parser->states[0].line, "missing (");
			f->pc = 8;
			return;
		}
		if (parser->states[0].token == XS_TOKEN_LEFT_PARENTHESIS) {
			f->u1 |= mxShorthandFlag | mxMethodFlag;
			fxParserCallTokenFlag(parser, K_FUNCTION_EXPR, f->i2, XS_NO_TOKEN, mxSuperFlag | f->u1);
			f->pc = 8;
			return;
		}
		if (parser->states[0].token == XS_TOKEN_COLON) {
			fxGetNextToken(parser);
			fxParserCall(parser, K_ASSIGN, f->i2);
			f->pc = 8;
			return;
		}
		if (f->t1 == XS_TOKEN_PROPERTY) {
			f->u1 |= mxShorthandFlag;
			fxPushSymbol(parser, f->s0);
			if (parser->states[0].token == XS_TOKEN_ASSIGN) {
				fxPushNodeStruct(parser, 1, XS_TOKEN_ACCESS, f->i2);
				fxGetNextToken(parser);
				fxParserCall(parser, K_ASSIGN, f->i2);
				P_RESUME(4);
			}
			else if (f->t0 == XS_TOKEN_IDENTIFIER) {
				fxPushNodeStruct(parser, 1, XS_TOKEN_ACCESS, f->i2);
				f->pc = 8;
				return;
			}
			else {
				fxReportParserError(parser, parser->states[0].line, "invalid identifier");
				fxPushNodeStruct(parser, 0, XS_TOKEN_UNDEFINED, f->i2);
				f->pc = 8;
				return;
			}
		}
		fxReportParserError(parser, parser->states[0].line, "missing :");
		fxPushNodeStruct(parser, 0, XS_TOKEN_UNDEFINED, f->i2);
		f->pc = 8;
		return;
	case 4:
		fxPushNodeStruct(parser, 2, XS_TOKEN_BINDING, f->i2);
		f->pc = 8;
		return;
	case 8:
		if (f->t1 != XS_NO_TOKEN) {
			fxPushNodeStruct(parser, 2, f->t1, f->i2);
			parser->root->flags |= f->u1;
			fxCheckUniqueProperty(parser, f->n0, parser->root);
		}
		f->i1++;
		if (parser->states[0].token == XS_TOKEN_RIGHT_BRACE) {
			f->pc = 1;
			return;
		}
		fxMatchToken(parser, XS_TOKEN_COMMA);
		f->pc = 1;
		return;
	}
	fxParserReturn(parser);
}

void fxJSONValueStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		switch (parser->states[0].token) {
		case XS_TOKEN_FALSE:
		case XS_TOKEN_TRUE:
		case XS_TOKEN_NULL:
			fxPushNodeStruct(parser, 0, parser->states[0].token, parser->states[0].line);
			fxGetNextTokenJSON(parser);
			break;
		case XS_TOKEN_INTEGER:
			fxPushIntegerNode(parser, parser->states[0].integer, parser->states[0].line);
			fxGetNextTokenJSON(parser);
			break;
		case XS_TOKEN_NUMBER:
			fxPushNumberNode(parser, parser->states[0].number, parser->states[0].line);
			fxGetNextTokenJSON(parser);
			break;
		case XS_TOKEN_STRING:
			fxPushStringNode(parser, parser->states[0].stringLength, parser->states[0].string, parser->states[0].line);
			fxGetNextTokenJSON(parser);
			break;
		case XS_TOKEN_LEFT_BRACE:
			fxParserCall(parser, K_JSON_OBJECT, parser->states[0].line);
			P_RESUME(1);
		case XS_TOKEN_LEFT_BRACKET:
			fxParserCall(parser, K_JSON_ARRAY, parser->states[0].line);
			P_RESUME(1);
		default:
			fxPushNULL(parser);
			fxReportParserError(parser, parser->states[0].line, "invalid value");
			break;
		}
		break;
	case 1:
		break;
	}
	fxParserReturn(parser);
}

void fxJSONObjectStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->i1 = 0;
		f->i0 = parser->states[0].line;
		fxGetNextTokenJSON(parser);
		f->pc = 1;
		return;
	case 1:
		if (parser->states[0].token == XS_TOKEN_RIGHT_BRACE) {
			if (parser->states[0].token != XS_TOKEN_RIGHT_BRACE)
				fxReportParserError(parser, parser->states[0].line, "missing }");
			fxGetNextTokenJSON(parser);
			fxPushNodeList(parser, f->i1);
			fxPushNodeStruct(parser, 1, XS_TOKEN_OBJECT, f->i0);
			break;
		}
		if (parser->states[0].token != XS_TOKEN_STRING) {
			fxReportParserError(parser, parser->states[0].line, "missing name");
			if (parser->states[0].token != XS_TOKEN_RIGHT_BRACE)
				fxReportParserError(parser, parser->states[0].line, "missing }");
			fxGetNextTokenJSON(parser);
			fxPushNodeList(parser, f->i1);
			fxPushNodeStruct(parser, 1, XS_TOKEN_OBJECT, f->i0);
			break;
		}
		fxPushStringNode(parser, parser->states[0].stringLength, parser->states[0].string, parser->states[0].line);
		fxGetNextTokenJSON(parser);
		if (parser->states[0].token != XS_TOKEN_COLON) {
			fxReportParserError(parser, parser->states[0].line, "missing :");
			if (parser->states[0].token != XS_TOKEN_RIGHT_BRACE)
				fxReportParserError(parser, parser->states[0].line, "missing }");
			fxGetNextTokenJSON(parser);
			fxPushNodeList(parser, f->i1);
			fxPushNodeStruct(parser, 1, XS_TOKEN_OBJECT, f->i0);
			break;
		}
		fxGetNextTokenJSON(parser);
		fxParserCall(parser, K_JSON_VALUE, f->i0);
		P_RESUME(2);
	case 2:
		fxPushNodeStruct(parser, 2, XS_TOKEN_PROPERTY_AT, parser->states[0].line);
		f->i1++;
		if (parser->states[0].token != XS_TOKEN_COMMA) {
			f->pc = 1;
			return;
		}
		fxGetNextTokenJSON(parser);
		f->pc = 1;
		return;
	}
	fxParserReturn(parser);
}

void fxJSONArrayStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->i1 = 0;
		f->i0 = parser->states[0].line;
		fxGetNextTokenJSON(parser);
		f->pc = 1;
		return;
	case 1:
		if (parser->states[0].token == XS_TOKEN_RIGHT_BRACKET) {
			if (parser->states[0].token != XS_TOKEN_RIGHT_BRACKET)
				fxReportParserError(parser, parser->states[0].line, "missing ]");
			fxGetNextTokenJSON(parser);
			fxPushNodeList(parser, f->i1);
			fxPushNodeStruct(parser, 1, XS_TOKEN_ARRAY, f->i0);
			break;
		}
		fxParserCall(parser, K_JSON_VALUE, f->i0);
		P_RESUME(2);
	case 2:
		f->i1++;
		if (parser->states[0].token != XS_TOKEN_COMMA) {
			f->pc = 1;
			return;
		}
		fxGetNextTokenJSON(parser);
		f->pc = 1;
		return;
	}
	fxParserReturn(parser);
}

/* xs_no_recursion: public entry points that still-native leaf code
   (fxExportDeclaration, fxCheckStrictFunction, ...) calls; they forward
   into the trampoline. */



void fxForStatement(txParser* parser) { fxParserCall(parser, K_FOR, parser->states[0].line); }
void fxSwitchStatement(txParser* parser) { fxParserCall(parser, K_SWITCH, parser->states[0].line); }
void fxTryStatement(txParser* parser) { fxParserCall(parser, K_TRY, parser->states[0].line); }
void fxWhileStatement(txParser* parser) { fxParserCall(parser, K_WHILE, parser->states[0].line); }
void fxIfStatement(txParser* parser) { fxParserCall(parser, K_IF, parser->states[0].line); }
void fxWithStatement(txParser* parser) { fxParserCall(parser, K_WITH, parser->states[0].line); }
void fxDoStatement(txParser* parser) { fxParserCall(parser, K_DO, parser->states[0].line); }
void fxArrowExpression(txParser* parser, txUnsigned flag) { fxParserCallFlag(parser, K_ARROW, parser->states[0].line, flag); }
void fxObjectExpression(txParser* parser) { fxParserCall(parser, K_OBJECT_EXPR, parser->states[0].line); }
void fxArrayExpression(txParser* parser) { fxParserCall(parser, K_ARRAY_EXPR, parser->states[0].line); }
void fxNewExpression(txParser* parser) { fxParserCall(parser, K_NEW_EXPR, parser->states[0].line); }
void fxTemplateExpression(txParser* parser) { fxParserCall(parser, K_TEMPLATE_EXPR, parser->states[0].line); }
void fxGroupExpression(txParser* parser, txUnsigned flag) { fxParserCallFlag(parser, K_GROUP_EXPR, parser->states[0].line, flag); }
void fxLiteralExpression(txParser* parser, txUnsigned flag) { fxParserCallFlag(parser, K_LITERAL, parser->states[0].line, flag); }
void fxBody(txParser* parser) { fxParserCall(parser, K_BODY, parser->states[0].line); }
void fxStatements(txParser* parser) { fxParserCall(parser, K_STATEMENTS, parser->states[0].line); }
void fxBlock(txParser* parser) { fxParserCall(parser, K_BLOCK, parser->states[0].line); }
void fxStatement(txParser* parser, txInteger blockIt) { fxParserCallParam(parser, K_STATEMENT, parser->states[0].line, blockIt, 0); }
void fxParameters(txParser* parser) { fxParserCall(parser, K_PARAMETERS, parser->states[0].line); }
void fxJSXAttributeValue(txParser* parser) { fxParserCall(parser, K_JSX_ATTRIBUTE_VALUE, parser->states[0].line); }
void fxJSXElement(txParser* parser) { fxParserCall(parser, K_JSX_ELEMENT, parser->states[0].line); }

/* xs_no_recursion: out-param forwarders. The trampoline publishes the
   function/class name through parser->outSymbol on return (see
   fxParserReturn); theSymbol is written back after the pump drains. */
void fxClassExpression(txParser* parser, txInteger theLine, txSymbol** theSymbol)
{
	fxParserCallClassFlag(parser, K_CLASS_EXPR, theLine, 0, theSymbol);
}

void fxVariableStatement(txParser* parser, txToken theToken, txUnsigned flags)
{
	fxParserCallTokenFlag(parser, K_VARIABLE_STATEMENT, parser->states[0].line, theToken, flags);
}

void fxCommaExpression(txParser* parser)
{
	fxParserCall(parser, K_COMMA, parser->states[0].line);
}

void fxAssignmentExpression(txParser* parser)
{
	fxParserCall(parser, K_ASSIGN, parser->states[0].line);
}

void fxFunctionExpression(txParser* parser, txInteger theLine, txSymbol** theSymbol, txUnsigned flag)
{
	fxParserCallClassFlag(parser, K_FUNCTION_EXPR, theLine, flag, theSymbol);
}

void fxGeneratorExpression(txParser* parser, txInteger theLine, txSymbol** theSymbol, txUnsigned flag)
{
	fxParserCallClassFlag(parser, K_GENERATOR_EXPR, theLine, flag, theSymbol);
}

/* xs_no_recursion: leaf statement bodies (no recursion — they only build
   nodes); restored native so fxStatementStep can call them from inside
   the pump. fxReturnStatement/fxThrowStatement's expression sub-call is
   a trampoline forwarder, so this stays pump-safe. */
void fxBreakStatement(txParser* parser)
{
	txInteger aLine = parser->states[0].line;
	fxMatchToken(parser, XS_TOKEN_BREAK);
	if ((!parser->states[0].crlf) && (parser->states[0].token == XS_TOKEN_IDENTIFIER)) {
		fxPushSymbol(parser, parser->states[0].symbol);
		fxGetNextToken(parser);
	}
	else {
		fxPushNULL(parser);
	}
	fxPushNodeStruct(parser, 1, XS_TOKEN_BREAK, aLine);
}

void fxContinueStatement(txParser* parser)
{
	txInteger aLine = parser->states[0].line;
	fxMatchToken(parser, XS_TOKEN_CONTINUE);
	if ((!parser->states[0].crlf) && (parser->states[0].token == XS_TOKEN_IDENTIFIER)) {
		fxPushSymbol(parser, parser->states[0].symbol);
		fxGetNextToken(parser);
	}
	else {
		fxPushNULL(parser);
	}
	fxPushNodeStruct(parser, 1, XS_TOKEN_CONTINUE, aLine);
}

void fxDebuggerStatement(txParser* parser)
{
	txInteger aLine = parser->states[0].line;
	fxMatchToken(parser, XS_TOKEN_DEBUGGER);
	fxPushNodeStruct(parser, 0, XS_TOKEN_DEBUGGER, aLine);
}

void fxReturnStatement(txParser* parser)
{
	txInteger aLine = parser->states[0].line;
	fxMatchToken(parser, XS_TOKEN_RETURN);
	if ((!parser->states[0].crlf) && (gxTokenFlags[parser->states[0].token] & XS_TOKEN_BEGIN_EXPRESSION)) {
		fxCommaExpression(parser);
	}
	else {
		fxPushNULL(parser);
	}
	fxPushNodeStruct(parser, 1, XS_TOKEN_RETURN, aLine);
}

void fxThrowStatement(txParser* parser)
{
	txInteger aLine = parser->states[0].line;
	fxMatchToken(parser, XS_TOKEN_THROW);
	if ((!parser->states[0].crlf) && (gxTokenFlags[parser->states[0].token] & XS_TOKEN_BEGIN_EXPRESSION)) {
		fxCommaExpression(parser);
	}
	else {
		fxReportParserError(parser, parser->states[0].line, "missing expression");
		fxPushNodeStruct(parser, 0, XS_TOKEN_UNDEFINED, aLine);
	}
	fxPushNodeStruct(parser, 1, XS_TOKEN_THROW, aLine);
}

/* xs_no_recursion: explicit work stack for the FromExpression family and
   fxCheckStrictBinding. These run on already-parsed trees from inside the
   trampoline pump, so native recursion would break the frame chain.
   Entries are c_malloc'd and freed as the walk unwinds; on the parser's
   error-abort path (fxAbort via mxCatchParser) fxFromExprReset runs from
   the catch handlers, and the aborted parse's chunk arena is freed
   wholesale, so leaked entries are bounded per abort. */
typedef struct sxFromExprNode sxFromExprNode;

enum {
	kFromDone = 0,		/* unused; keeps zero-initialized entries inert */
	kFromPlain,			/* plain pending-node worklist (fxCheckStrictBinding items) */
	kFromSetField,		/* *field = child result, deliver the parent node up */
	kFromBindingItems,	/* ARRAY_BINDING/OBJECT_BINDING item list loop */
	kFromArrayExpr,		/* ARRAY expression container (fxArrayBindingFromExpression) */
	kFromObjectExpr,	/* OBJECT expression container (fxObjectBindingFromExpression) */
	kFromAssign,		/* ASSIGN node -> BINDING node rewrite */
	kFromRestFinal		/* standalone rest element (fxRestBindingFromExpression) */
};

struct sxFromExprNode {
	sxFromExprNode* next;
	int op;
	int stage;			/* 0 = between children, 1 = plain child pending, 2 = rest child pending */
	txNode** field;		/* set-field address / container item address */
	txNode* node;		/* context node (parent, container, rest node) */
	txNode* cursor;		/* current item / property cursor */
	txUnsigned flags;	/* rest flag / pending property-binding kind */
};

static void fxFromExprPushOp(txParser* parser, int op, txNode** field, txNode* node, txNode* cursor, txUnsigned flags)
{
	sxFromExprNode* entry = (sxFromExprNode*)c_malloc(sizeof(sxFromExprNode));
	if (!entry)
		fxAbort(parser->console, XS_NOT_ENOUGH_MEMORY_EXIT);
	entry->next = parser->fromExprStack;
	entry->op = op;
	entry->stage = 0;
	entry->field = field;
	entry->node = node;
	entry->cursor = cursor;
	entry->flags = flags;
	parser->fromExprStack = entry;
}

static void fxFromExprPushNode(txParser* parser, txNode* node)
{
	fxFromExprPushOp(parser, kFromPlain, C_NULL, node, C_NULL, 0);
}

static txNode* fxFromExprPopNode(txParser* parser)
{
	sxFromExprNode* entry = parser->fromExprStack;
	txNode* node;
	if (!entry)
		return C_NULL;
	node = entry->node;
	parser->fromExprStack = entry->next;
	c_free(entry);
	return node;
}

static void fxFromExprDrain(txParser* parser)
{
	while (parser->fromExprStack) {
		sxFromExprNode* entry = parser->fromExprStack;
		parser->fromExprStack = entry->next;
		c_free(entry);
	}
}

void fxFromExprReset(txParser* parser)
{
	fxFromExprDrain(parser);
}

/* The shared driver. Enters at (theNode, theToken) with an empty slice
   above `base`; returns the binding result for theNode (C_NULL on the
   SyntaxError paths). Any frames left above `base` are drained. */
static txNode* fxFromExprRunBase(txParser* parser, txNode* theNode, txToken theToken, sxFromExprNode* base);
static txNode* fxFromExprRun(txParser* parser, txNode* theNode, txToken theToken)
{
	return fxFromExprRunBase(parser, theNode, theToken, parser->fromExprStack);
}

/* base is the stack level the caller expects deliver-up to stop at: callers
   that pre-push a container frame (fxArrayBindingFromExpression, etc.) pass
   the stack pointer captured BEFORE the push so the frame runs to completion
   and its binding result is returned. */
static txNode* fxFromExprRunBase(txParser* parser, txNode* theNode, txToken theToken, sxFromExprNode* base)
{
	sxFromExprNode* entry;
	txNode* node = theNode;
	txNode* deliver;
	int descend;

	for (;;) {
		descend = 0;
		deliver = C_NULL;
		if (node) {
			txToken aToken = (node->description) ? node->description->token : XS_NO_TOKEN;
		again:
			if (aToken == XS_TOKEN_EXPRESSIONS) {
				txNode* item = ((txExpressionsNode*)node)->items->first;
				if (item && !item->next) {
					aToken = (item->description) ? item->description->token : XS_NO_TOKEN;
					if ((aToken == XS_TOKEN_ACCESS) || (aToken == XS_TOKEN_MEMBER) || (aToken == XS_TOKEN_MEMBER_AT) || (aToken == XS_TOKEN_PRIVATE_MEMBER) || (aToken == XS_TOKEN_UNDEFINED) || (aToken == XS_TOKEN_EXPRESSIONS)) {
						item->next = node->next;
						node = item;
						goto again;
					}
					/* else: falls through with the unwrapped token */
				}
				else {
					/* zero or multiple items: stock falls through to the
					   token checks, which never match EXPRESSIONS */
					aToken = XS_TOKEN_EXPRESSIONS;
				}
			}
			switch (aToken) {
			case XS_TOKEN_BINDING:
				fxFromExprPushOp(parser, kFromSetField, &((txBindingNode*)node)->target, node, C_NULL, 0);
				node = ((txBindingNode*)node)->target;
				descend = 1;
				break;
			case XS_TOKEN_ARRAY_BINDING:
			case XS_TOKEN_OBJECT_BINDING: {
				txNodeList* list = (aToken == XS_TOKEN_ARRAY_BINDING) ? ((txArrayBindingNode*)node)->items : ((txObjectBindingNode*)node)->items;
				txNode* item = list ? list->first : C_NULL;
				if (item) {
					fxFromExprPushOp(parser, kFromBindingItems, &(list->first), node, item, 0);
					node = item;
					descend = 1;
				}
				else
					deliver = node;
				break;
			}
			case XS_TOKEN_PROPERTY_BINDING:
				fxFromExprPushOp(parser, kFromSetField, &((txPropertyBindingNode*)node)->binding, node, C_NULL, 0);
				node = ((txPropertyBindingNode*)node)->binding;
				descend = 1;
				break;
			case XS_TOKEN_PROPERTY_BINDING_AT:
				fxFromExprPushOp(parser, kFromSetField, &((txPropertyBindingAtNode*)node)->binding, node, C_NULL, 0);
				node = ((txPropertyBindingAtNode*)node)->binding;
				descend = 1;
				break;
			case XS_TOKEN_REST_BINDING:
				fxFromExprPushOp(parser, kFromSetField, &((txRestBindingNode*)node)->binding, node, C_NULL, 0);
				node = ((txRestBindingNode*)node)->binding;
				descend = 1;
				break;
			case XS_TOKEN_SKIP_BINDING:
				deliver = node;
				break;
			case XS_TOKEN_ACCESS:
				fxCheckStrictSymbol(parser, ((txAccessNode*)node)->symbol);
				if (theToken == XS_TOKEN_ACCESS) {
					deliver = node;
					break;
				}
				fxPushSymbol(parser, ((txAccessNode*)node)->symbol);
				fxPushNodeStruct(parser, 1, theToken, ((txAccessNode*)node)->line);
				deliver = fxPopNode(parser);
				break;
			case XS_TOKEN_MEMBER:
			case XS_TOKEN_MEMBER_AT:
			case XS_TOKEN_PRIVATE_MEMBER:
			case XS_TOKEN_UNDEFINED:
				deliver = node;
				break;
			case XS_TOKEN_ASSIGN:
				fxFromExprPushOp(parser, kFromAssign, C_NULL, node, C_NULL, 0);
				node = ((txAssignNode*)node)->reference;
				descend = 1;
				break;
			case XS_TOKEN_ARRAY: {
				txNodeList* list = ((txArrayNode*)node)->items;
				txNode* item = list ? list->first : C_NULL;
				if (item) {
					fxFromExprPushOp(parser, kFromArrayExpr, &(list->first), node, item, 0);
					node = item;
					descend = 1;
				}
				else {
					fxPushNode(parser, (txNode*)list);
					fxPushNodeStruct(parser, 1, XS_TOKEN_ARRAY_BINDING, node->line);
					deliver = fxPopNode(parser);
				}
				break;
			}
			case XS_TOKEN_OBJECT: {
				txNodeList* list = ((txObjectNode*)node)->items;
				txNode* item = list ? list->first : C_NULL;
				if (item) {
					fxFromExprPushOp(parser, kFromObjectExpr, &(list->first), node, item, 0);
					node = item;
					descend = 1;
				}
				else {
					fxPushNode(parser, (txNode*)list);
					fxPushNodeStruct(parser, 1, XS_TOKEN_OBJECT_BINDING, node->line);
					deliver = fxPopNode(parser);
				}
				break;
			}
			default:
				deliver = C_NULL;
				break;
			}
		}			if (!descend) {
				/* deliver upward through the stack frames */
				for (;;) {
				if (parser->fromExprStack == base)
					return deliver;
				entry = parser->fromExprStack;
				switch (entry->op) {
				case kFromSetField:
					*entry->field = deliver;
					deliver = entry->node;
					parser->fromExprStack = entry->next;
					c_free(entry);
					break;
				case kFromBindingItems: {
					/* xs_no_recursion: same stage-0 rule as the container
					   frames — this frame is pushed with its first item
					   already pending, so the first deliver must be wired
					   like any later one; ignoring it re-converted the
					   (already mutated) item. */
					if (!deliver)
						goto propagate_error;
					deliver->next = entry->cursor->next;
					*entry->field = deliver;
					entry->field = &deliver->next;
					entry->cursor = deliver->next;
					while (entry->cursor) {
						txNode* item = entry->cursor;
						/* built binding trees contain no SPREAD items: a
						   stray one falls to the driver's default case and
						   errors, matching stock. */
						entry->stage = 1;
						node = item;
						descend = 1;
						break;
					}
					if (!descend) {
						deliver = entry->node;
						parser->fromExprStack = entry->next;
						c_free(entry);
					}
					break;
				}
				case kFromAssign:
					if (!deliver)
						goto propagate_error;
					((txBindingNode*)entry->node)->description = &gxTokenDescriptions[XS_TOKEN_BINDING];
					((txBindingNode*)entry->node)->target = deliver;
					((txBindingNode*)entry->node)->initializer = ((txAssignNode*)entry->node)->value;
					deliver = entry->node;
					parser->fromExprStack = entry->next;
					c_free(entry);
					break;
				case kFromArrayExpr: {
					txNode* item;
					if (entry->stage == 2) {
						/* rest element completed by its kFromRestFinal
						   frame (item mutated in place); finish here */
						fxPushNode(parser, (txNode*)((txArrayNode*)entry->node)->items);
						fxPushNodeStruct(parser, 1, XS_TOKEN_ARRAY_BINDING, entry->node->line);
						deliver = fxPopNode(parser);
						parser->fromExprStack = entry->next;
						c_free(entry);
						break;
					}
					/* xs_no_recursion: stage 0 AND 1 both mean "the child that
					   just returned is entry->cursor" — these frames are pushed
					   with the first item already pending, so the first deliver
					   must be wired like any later one. Ignoring it re-converted
					   the (already mutated) item and errored or corrupted. */
					if (entry->stage != 2) {
						if (!deliver)
							goto propagate_error;
						deliver->next = entry->cursor->next;
						*entry->field = deliver;
						entry->field = &deliver->next;
						entry->cursor = deliver->next;
					}
					item = entry->cursor;
					while (item) {
						if (!item->description)
							goto propagate_error;
						if (item->description->token == XS_TOKEN_SPREAD) {
							if (entry->node->flags & mxElisionFlag)
								goto propagate_error;
							fxFromExprPushOp(parser, kFromRestFinal, C_NULL, item, entry->node, 0);
							entry->stage = 2;
							node = ((txSpreadNode*)item)->expression;
							descend = 1;
							break;
						}
						if (item->description->token == XS_TOKEN_ELISION) {
							item->description = &gxTokenDescriptions[XS_TOKEN_SKIP_BINDING];
							entry->field = &item->next;
							item = item->next;
							continue;
						}
						entry->stage = 1;
						entry->cursor = item;
						node = item;
						descend = 1;
						break;
					}
					if (!descend) {
						fxPushNode(parser, (txNode*)((txArrayNode*)entry->node)->items);
						fxPushNodeStruct(parser, 1, XS_TOKEN_ARRAY_BINDING, entry->node->line);
						deliver = fxPopNode(parser);
						parser->fromExprStack = entry->next;
						c_free(entry);
					}
					break;
				}
				case kFromObjectExpr: {
					txNode* property;
					if (entry->stage == 2) {
						/* rest element completed by its kFromRestFinal
						   frame; finish here (stock ORs mxSpreadFlag) */
						fxPushNode(parser, (txNode*)((txObjectNode*)entry->node)->items);
						fxPushNodeStruct(parser, 1, XS_TOKEN_OBJECT_BINDING, entry->node->line);
						parser->root->flags |= mxSpreadFlag;
						deliver = fxPopNode(parser);
						parser->fromExprStack = entry->next;
						c_free(entry);
						break;
					}
					/* xs_no_recursion: KEEP the stage==1 gate here, unlike
					   kFromArrayExpr: the driver descends into the raw
					   PROPERTY node (no driver case), so the first deliver
					   is C_NULL by design and the property scan below is
					   what descends into the value. Wiring a NULL would
					   abort legitimate conversions. */
					if (entry->stage == 1) {
						if (!deliver)
							goto propagate_error;
						property = entry->cursor;
						property->description = &gxTokenDescriptions[entry->flags];
						if (entry->flags == XS_TOKEN_PROPERTY_BINDING)
							((txPropertyBindingNode*)property)->binding = deliver;
						else
							((txPropertyBindingAtNode*)property)->binding = deliver;
						entry->cursor = property->next;
					}
					property = entry->cursor;
					while (property) {
						if (!property->description)
							goto propagate_error;
						if (property->description->token == XS_TOKEN_PROPERTY) {
							entry->stage = 1;
							entry->flags = XS_TOKEN_PROPERTY_BINDING;
							entry->cursor = property;
							node = ((txPropertyNode*)property)->value;
							descend = 1;
							break;
						}
						if (property->description->token == XS_TOKEN_PROPERTY_AT) {
							entry->stage = 1;
							entry->flags = XS_TOKEN_PROPERTY_BINDING_AT;
							entry->cursor = property;
							node = ((txPropertyAtNode*)property)->value;
							descend = 1;
							break;
						}
						if (property->description->token == XS_TOKEN_SPREAD) {
							if (property->flags & mxElisionFlag) {
								fxReportParserError(parser, property->line, "invalid comma after rest");
								goto propagate_error;
							}
							fxFromExprPushOp(parser, kFromRestFinal, C_NULL, property, entry->node, 1);
							entry->stage = 2;
							node = ((txSpreadNode*)property)->expression;
							descend = 1;
							break;
						}
						property = property->next;
					}
					if (!descend) {
						fxPushNode(parser, (txNode*)((txObjectNode*)entry->node)->items);
						fxPushNodeStruct(parser, 1, XS_TOKEN_OBJECT_BINDING, entry->node->line);
						deliver = fxPopNode(parser);
						parser->fromExprStack = entry->next;
						c_free(entry);
					}
					break;
				}
				case kFromRestFinal: {
					/* deliver is the rest element's binding; the stack above
					   base is [.. container-frame, rest-frame] with the rest
					   frame on top. Validate, mutate, then let the container
					   frame see entry->node already done via its cursor. */
					txNode* container = entry->cursor;
					if (!deliver)
						goto propagate_error;
					if (deliver->description->token == XS_TOKEN_BINDING) {
						fxReportParserError(parser, parser->states[0].line, "invalid rest");
						goto propagate_error;
					}
					if (entry->flags && ((deliver->description->token == XS_TOKEN_ARRAY_BINDING) || (deliver->description->token == XS_TOKEN_OBJECT_BINDING))) {
						fxReportParserError(parser, parser->states[0].line, "invalid rest");
						goto propagate_error;
					}
					entry->node->description = &gxTokenDescriptions[XS_TOKEN_REST_BINDING];
					((txRestBindingNode*)entry->node)->binding = deliver;
					(void)container;
					parser->fromExprStack = entry->next;
					c_free(entry);
					break;
				}
				default:
					deliver = C_NULL;
					parser->fromExprStack = entry->next;
					c_free(entry);
					break;
				}
				if (descend)
					break;
			}
		}
		continue;
	propagate_error:
		while (parser->fromExprStack != base) {
			entry = parser->fromExprStack;
			parser->fromExprStack = entry->next;
			c_free(entry);
		}
		return C_NULL;
	}
}

/* xs_no_recursion: leaf item check used by fxCheckStrictBinding's
   container cases: pushes the list items as plain work. */
static void fxCheckStrictBindingItems(txParser* parser, txNodeList* list)
{
	txNode* item = list ? list->first : C_NULL;
	while (item) {
		txNode* next = item->next;
		fxFromExprPushNode(parser, item);
		item = next;
	}
}

void fxCheckStrictBinding(txParser* parser, txNode* node)
{
	while (node && node->description) {
		switch (node->description->token) {
		case XS_TOKEN_ACCESS:
			fxCheckStrictSymbol(parser, ((txAccessNode*)node)->symbol);
			node = fxFromExprPopNode(parser);
			break;
		case XS_TOKEN_ARG:
		case XS_TOKEN_CONST:
		case XS_TOKEN_LET:
		case XS_TOKEN_USING:
		case XS_TOKEN_VAR:
			fxCheckStrictSymbol(parser, ((txDeclareNode*)node)->symbol);
			node = fxFromExprPopNode(parser);
			break;
		case XS_TOKEN_BINDING:
			node = ((txBindingNode*)node)->target;
			break;
		case XS_TOKEN_ARRAY_BINDING:
			fxCheckStrictBindingItems(parser, ((txArrayBindingNode*)node)->items);
			node = fxFromExprPopNode(parser);
			break;
		case XS_TOKEN_OBJECT_BINDING:
			fxCheckStrictBindingItems(parser, ((txObjectBindingNode*)node)->items);
			node = fxFromExprPopNode(parser);
			break;
		case XS_TOKEN_PARAMS_BINDING:
			fxCheckStrictBindingItems(parser, ((txParamsBindingNode*)node)->items);
			node = fxFromExprPopNode(parser);
			break;
		case XS_TOKEN_PROPERTY_BINDING:
			node = ((txPropertyBindingNode*)node)->binding;
			break;
		case XS_TOKEN_PROPERTY_BINDING_AT:
			node = ((txPropertyBindingAtNode*)node)->binding;
			break;
		case XS_TOKEN_REST_BINDING:
			node = ((txRestBindingNode*)node)->binding;
			break;
		default:
			node = fxFromExprPopNode(parser);
			break;
		}
	}
}

txNode* fxBindingFromExpression(txParser* parser, txNode* theNode, txToken theToken)
{
	return fxFromExprRun(parser, theNode, theToken);
}

txNode* fxArrayBindingFromExpression(txParser* parser, txNode* theNode, txToken theToken)
{
	sxFromExprNode* base = parser->fromExprStack;
	txNodeList* list = ((txArrayNode*)theNode)->items;
	txNode* item = list ? list->first : C_NULL;
	fxFromExprPushOp(parser, kFromArrayExpr, list ? &(list->first) : C_NULL, theNode, item, 0);
	return fxFromExprRunBase(parser, item, theToken, base);
}

txNode* fxObjectBindingFromExpression(txParser* parser, txNode* theNode, txToken theToken)
{
	sxFromExprNode* base = parser->fromExprStack;
	txNodeList* list = ((txObjectNode*)theNode)->items;
	txNode* item = list ? list->first : C_NULL;
	fxFromExprPushOp(parser, kFromObjectExpr, list ? &(list->first) : C_NULL, theNode, item, 0);
	return fxFromExprRunBase(parser, item, theToken, base);
}

txNode* fxRestBindingFromExpression(txParser* parser, txNode* theNode, txToken theToken, txUnsigned flag)
{
	txNode* binding;
	txNode* expression;
	if (theNode->next) {
		parser->errorSymbol = parser->SyntaxErrorSymbol;
		return NULL;
	}
	expression = ((txSpreadNode*)theNode)->expression;
	if (!expression)
		return NULL;
	sxFromExprNode* base = parser->fromExprStack;
	fxFromExprPushOp(parser, kFromRestFinal, C_NULL, theNode, C_NULL, flag);
	/* the frame validates the child (stock rules) and mutates theNode to
	   REST_BINDING; it returns theNode or C_NULL (error propagated) */
	return fxFromExprRunBase(parser, expression, theToken, base);
}

/* =====================================================================
   xs_no_recursion: expression-ladder step routines
   ===================================================================== */

void fxAssignmentExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		if (parser->states[0].token == XS_TOKEN_YIELD) {
			fxParserCall(parser, K_YIELD_EXPR, parser->states[0].line);
			P_RESUME(1);
		}
		fxParserCall(parser, K_CONDITIONAL, parser->states[0].line);
		P_RESUME(2);
	case 1:
		break;
	case 2:
		if (!(gxTokenFlags[parser->states[0].token] & XS_TOKEN_ASSIGN_EXPRESSION))
			break;
		f->t0 = parser->states[0].token;
		f->i0 = parser->states[0].line;
		if (!fxCheckReference(parser, f->t0))
			fxReportParserError(parser, parser->states[0].line, "no reference");
		fxGetNextToken(parser);
		fxParserCall(parser, K_ASSIGN, parser->states[0].line);
		P_RESUME(3);
	case 3:
		fxPushNodeStruct(parser, 2, f->t0, f->i0);
		f->pc = 2;
		return;
	}
	fxParserReturn(parser);
}

void fxConditionalExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		fxParserCall(parser, K_COALESCE, parser->states[0].line);
		P_RESUME(1);
	case 1:
		if (parser->states[0].token != XS_TOKEN_QUESTION_MARK)
			break;
		f->i0 = parser->states[0].line;
		fxCheckArrowFunction(parser, 1);
		fxGetNextToken(parser);
		f->u0 = parser->flags & mxForFlag;
		parser->flags &= ~mxForFlag;
		fxParserCall(parser, K_ASSIGN, parser->states[0].line);
		P_RESUME(2);
	case 2:
		parser->flags |= f->u0;
		fxMatchToken(parser, XS_TOKEN_COLON);
		fxParserCall(parser, K_ASSIGN, parser->states[0].line);
		P_RESUME(3);
	case 3:
		fxPushNodeStruct(parser, 3, XS_TOKEN_QUESTION_MARK, f->i0);
		break;
	}
	fxParserReturn(parser);
}

void fxCoalesceExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		fxParserCall(parser, K_OR, parser->states[0].line);
		P_RESUME(1);
	case 1:
		if (parser->states[0].token != XS_TOKEN_COALESCE)
			break;
		f->i0 = parser->states[0].line;
		fxGetNextToken(parser);
		fxParserCall(parser, K_OR, parser->states[0].line);
		P_RESUME(2);
	case 2:
		fxCheckArrowFunction(parser, 2);
		fxPushNodeStruct(parser, 2, XS_TOKEN_COALESCE, f->i0);
		f->pc = 1;
		return;
	}
	fxParserReturn(parser);
}

void fxOrExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		fxParserCall(parser, K_AND, parser->states[0].line);
		P_RESUME(1);
	case 1:
		if (parser->states[0].token != XS_TOKEN_OR)
			break;
		f->i0 = parser->states[0].line;
		fxGetNextToken(parser);
		fxParserCall(parser, K_AND, parser->states[0].line);
		P_RESUME(2);
	case 2:
		fxCheckArrowFunction(parser, 2);
		fxPushNodeStruct(parser, 2, XS_TOKEN_OR, f->i0);
		f->pc = 1;
		return;
	}
	fxParserReturn(parser);
}

void fxAndExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		fxParserCall(parser, K_BIT_OR, parser->states[0].line);
		P_RESUME(1);
	case 1:
		if (parser->states[0].token != XS_TOKEN_AND)
			break;
		f->i0 = parser->states[0].line;
		fxGetNextToken(parser);
		fxParserCall(parser, K_BIT_OR, parser->states[0].line);
		P_RESUME(2);
	case 2:
		fxCheckArrowFunction(parser, 2);
		fxPushNodeStruct(parser, 2, XS_TOKEN_AND, f->i0);
		f->pc = 1;
		return;
	}
	fxParserReturn(parser);
}

void fxBitOrExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		fxParserCall(parser, K_BIT_XOR, parser->states[0].line);
		P_RESUME(1);
	case 1:
		if (parser->states[0].token != XS_TOKEN_BIT_OR)
			break;
		f->i0 = parser->states[0].line;
		fxGetNextToken(parser);
		fxParserCall(parser, K_BIT_XOR, parser->states[0].line);
		P_RESUME(2);
	case 2:
		fxCheckArrowFunction(parser, 2);
		fxPushNodeStruct(parser, 2, XS_TOKEN_BIT_OR, f->i0);
		f->pc = 1;
		return;
	}
	fxParserReturn(parser);
}

void fxBitXorExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		fxParserCall(parser, K_BIT_AND, parser->states[0].line);
		P_RESUME(1);
	case 1:
		if (parser->states[0].token != XS_TOKEN_BIT_XOR)
			break;
		f->i0 = parser->states[0].line;
		fxGetNextToken(parser);
		fxParserCall(parser, K_BIT_AND, parser->states[0].line);
		P_RESUME(2);
	case 2:
		fxCheckArrowFunction(parser, 2);
		fxPushNodeStruct(parser, 2, XS_TOKEN_BIT_XOR, f->i0);
		f->pc = 1;
		return;
	}
	fxParserReturn(parser);
}

void fxBitAndExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		fxParserCall(parser, K_EQUAL, parser->states[0].line);
		P_RESUME(1);
	case 1:
		if (parser->states[0].token != XS_TOKEN_BIT_AND)
			break;
		f->i0 = parser->states[0].line;
		fxGetNextToken(parser);
		fxParserCall(parser, K_EQUAL, parser->states[0].line);
		P_RESUME(2);
	case 2:
		fxCheckArrowFunction(parser, 2);
		fxPushNodeStruct(parser, 2, XS_TOKEN_BIT_AND, f->i0);
		f->pc = 1;
		return;
	}
	fxParserReturn(parser);
}

void fxEqualExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		fxParserCall(parser, K_RELATIONAL, parser->states[0].line);
		P_RESUME(1);
	case 1:
		if (!(gxTokenFlags[parser->states[0].token] & XS_TOKEN_EQUAL_EXPRESSION))
			break;
		f->t0 = parser->states[0].token;
		f->i0 = parser->states[0].line;
		fxGetNextToken(parser);
		fxParserCall(parser, K_RELATIONAL, parser->states[0].line);
		P_RESUME(2);
	case 2:
		fxCheckArrowFunction(parser, 2);
		fxPushNodeStruct(parser, 2, f->t0, f->i0);
		f->pc = 1;
		return;
	}
	fxParserReturn(parser);
}

void fxRelationalExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		if (parser->states[0].token == XS_TOKEN_PRIVATE_IDENTIFIER) {
			f->i0 = parser->states[0].line;
			fxPushSymbol(parser, parser->states[0].symbol);
			fxGetNextToken(parser);
			fxMatchToken(parser, XS_TOKEN_IN);
			if (parser->flags & mxForFlag)
				fxReportParserError(parser, parser->states[0].line, "invalid %s", gxTokenNames[XS_TOKEN_IN]);
			fxParserCall(parser, K_SHIFT, parser->states[0].line);
			P_RESUME(1);
		}
		fxParserCall(parser, K_SHIFT, parser->states[0].line);
		P_RESUME(2);
	case 1:
		fxCheckArrowFunction(parser, 2);
		fxPushNodeStruct(parser, 2, XS_TOKEN_PRIVATE_IDENTIFIER, f->i0);
		break;
	case 2:
		if ((parser->flags & mxForFlag) && ((parser->states[0].token == XS_TOKEN_IN) || fxIsKeyword(parser, parser->ofSymbol)))
			break;
		if (!(gxTokenFlags[parser->states[0].token] & XS_TOKEN_RELATIONAL_EXPRESSION))
			break;
		f->t0 = parser->states[0].token;
		f->i0 = parser->states[0].line;
		fxMatchToken(parser, f->t0);
		fxParserCall(parser, K_SHIFT, parser->states[0].line);
		P_RESUME(3);
	case 3:
		fxCheckArrowFunction(parser, 2);
		fxPushNodeStruct(parser, 2, f->t0, f->i0);
		f->pc = 2;
		return;
	}
	fxParserReturn(parser);
}

void fxShiftExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		fxParserCall(parser, K_ADDITIVE, parser->states[0].line);
		P_RESUME(1);
	case 1:
		if (!(gxTokenFlags[parser->states[0].token] & XS_TOKEN_SHIFT_EXPRESSION))
			break;
		f->t0 = parser->states[0].token;
		f->i0 = parser->states[0].line;
		fxGetNextToken(parser);
		fxParserCall(parser, K_ADDITIVE, parser->states[0].line);
		P_RESUME(2);
	case 2:
		fxCheckArrowFunction(parser, 2);
		fxPushNodeStruct(parser, 2, f->t0, f->i0);
		f->pc = 1;
		return;
	}
	fxParserReturn(parser);
}

void fxAdditiveExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		fxParserCall(parser, K_MULTIPLICATIVE, parser->states[0].line);
		P_RESUME(1);
	case 1:
		if (!(gxTokenFlags[parser->states[0].token] & XS_TOKEN_ADDITIVE_EXPRESSION))
			break;
		f->t0 = parser->states[0].token;
		f->i0 = parser->states[0].line;
		fxGetNextToken(parser);
		fxParserCall(parser, K_MULTIPLICATIVE, parser->states[0].line);
		P_RESUME(2);
	case 2:
		fxCheckArrowFunction(parser, 2);
		fxPushNodeStruct(parser, 2, f->t0, f->i0);
		f->pc = 1;
		return;
	}
	fxParserReturn(parser);
}

void fxMultiplicativeExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		fxParserCall(parser, K_EXPONENTIATION, parser->states[0].line);
		P_RESUME(1);
	case 1:
		if (!(gxTokenFlags[parser->states[0].token] & XS_TOKEN_MULTIPLICATIVE_EXPRESSION))
			break;
		f->t0 = parser->states[0].token;
		f->i0 = parser->states[0].line;
		fxGetNextToken(parser);
		fxParserCall(parser, K_EXPONENTIATION, parser->states[0].line);
		P_RESUME(2);
	case 2:
		fxCheckArrowFunction(parser, 2);
		fxPushNodeStruct(parser, 2, f->t0, f->i0);
		f->pc = 1;
		return;
	}
	fxParserReturn(parser);
}

void fxExponentiationExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
#ifdef XS_NR_PUMP_TRACE
		c_fprintf(stderr, "EXPO tok=%d (%s) unary=%d\n", (int)parser->states[0].token, gxTokenNames[parser->states[0].token], (int)((gxTokenFlags[parser->states[0].token] & XS_TOKEN_UNARY_EXPRESSION) != 0));
#endif
		if (gxTokenFlags[parser->states[0].token] & XS_TOKEN_UNARY_EXPRESSION) {
			fxParserCall(parser, K_UNARY, parser->states[0].line);
			P_RESUME(1);
		}
		fxParserCall(parser, K_PREFIX, parser->states[0].line);
		P_RESUME(2);
	case 1:
		break;
	case 2:
		if (!(gxTokenFlags[parser->states[0].token] & XS_TOKEN_EXPONENTIATION_EXPRESSION))
			break;
		f->t0 = parser->states[0].token;
		f->i0 = parser->states[0].line;
		fxGetNextToken(parser);
		fxParserCall(parser, K_EXPONENTIATION, parser->states[0].line);
		P_RESUME(3);
	case 3:
		fxCheckArrowFunction(parser, 2);
		fxPushNodeStruct(parser, 2, f->t0, f->i0);
		break;
	}
	fxParserReturn(parser);
}

void fxUnaryExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		if (!(gxTokenFlags[parser->states[0].token] & XS_TOKEN_UNARY_EXPRESSION)) {
			fxParserCall(parser, K_PREFIX, parser->states[0].line);
			P_RESUME(1);
		}
		f->t0 = parser->states[0].token;
		f->i0 = parser->states[0].line;
		fxCheckParserStack(parser, f->i0);
		fxMatchToken(parser, f->t0);
		fxParserCall(parser, K_UNARY, parser->states[0].line);
		P_RESUME(2);
	case 1:
		break;
	case 2:
		fxCheckArrowFunction(parser, 1);
		if (f->t0 == XS_TOKEN_ADD)
			fxPushNodeStruct(parser, 1, XS_TOKEN_PLUS, f->i0);
		else if (f->t0 == XS_TOKEN_SUBTRACT)
			fxPushNodeStruct(parser, 1, XS_TOKEN_MINUS, f->i0);
		else if (f->t0 == XS_TOKEN_DELETE) {
			if (!fxCheckReference(parser, f->t0))
				fxReportParserError(parser, parser->states[0].line, "no reference");
			fxPushNodeStruct(parser, 1, f->t0, f->i0);
		}
		else if (f->t0 == XS_TOKEN_AWAIT) {
			if ((parser->flags & mxGeneratorFlag) && !(parser->flags & mxYieldFlag))
				fxReportParserError(parser, parser->states[0].line, "invalid await");
			else
				parser->flags |= mxAwaitingFlag;
			fxPushNodeStruct(parser, 1, f->t0, f->i0);
		}
		else
			fxPushNodeStruct(parser, 1, f->t0, f->i0);
		break;
	}
	fxParserReturn(parser);
}

void fxPrefixExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		if (!(gxTokenFlags[parser->states[0].token] & XS_TOKEN_PREFIX_EXPRESSION)) {
			fxParserCall(parser, K_POSTFIX, parser->states[0].line);
			P_RESUME(1);
		}
		f->t0 = parser->states[0].token;
		f->i0 = parser->states[0].line;
		fxCheckParserStack(parser, f->i0);
		fxGetNextToken(parser);
		fxParserCall(parser, K_PREFIX, parser->states[0].line);
		P_RESUME(2);
	case 1:
		break;
	case 2:
		fxCheckArrowFunction(parser, 1);
		if (!fxCheckReference(parser, f->t0))
			fxReportParserError(parser, parser->states[0].line, "no reference");
		fxPushNodeStruct(parser, 1, f->t0, f->i0);
		parser->root->flags = mxExpressionNoValue;
		break;
	}
	fxParserReturn(parser);
}

void fxPostfixExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		fxParserCall(parser, K_CALL_CHAIN, parser->states[0].line);
		P_RESUME(1);
	case 1:
		if ((parser->states[0].crlf) || !(gxTokenFlags[parser->states[0].token] & XS_TOKEN_POSTFIX_EXPRESSION))
			break;
		fxCheckArrowFunction(parser, 1);
		if (!fxCheckReference(parser, parser->states[0].token))
			fxReportParserError(parser, parser->states[0].line, "no reference");
		fxPushNodeStruct(parser, 1, parser->states[0].token, parser->states[0].line);
		fxGetNextToken(parser);
		break;
	}
	fxParserReturn(parser);
}

void fxCallExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->i0 = parser->states[0].line;
		f->b0 = 0;
		fxParserCallFlag(parser, K_LITERAL, parser->states[0].line, 0);
		P_RESUME(1);
	case 1:
		if (!(gxTokenFlags[parser->states[0].token] & XS_TOKEN_CALL_EXPRESSION))
			break;
		fxCheckArrowFunction(parser, 1);
		f->pc = 2;
		return;
	case 2:
		f->i1 = parser->states[0].line;
		if (parser->states[0].token == XS_TOKEN_DOT) {
			fxGetNextToken(parser);
			if (parser->states[0].token == XS_TOKEN_IDENTIFIER) {
				fxPushSymbol(parser, parser->states[0].symbol);
				fxPushNodeStruct(parser, 2, XS_TOKEN_MEMBER, f->i1);
				fxGetNextToken(parser);
			}
			else if (parser->states[0].token == XS_TOKEN_PRIVATE_IDENTIFIER) {
				if (parser->root->flags & mxSuperFlag)
					fxReportParserError(parser, parser->states[0].line, "invalid super");
				fxPushSymbol(parser, parser->states[0].symbol);
				fxSwapNodes(parser);
				fxPushNodeStruct(parser, 2, XS_TOKEN_PRIVATE_MEMBER, f->i1);
				fxGetNextToken(parser);
			}
			else
				fxReportParserError(parser, parser->states[0].line, "missing property");
			f->pc = 2;
			return;
		}
		else if (parser->states[0].token == XS_TOKEN_LEFT_BRACKET) {
			fxGetNextToken(parser);
			fxParserCall(parser, K_COMMA, parser->states[0].line);
			P_RESUME(3);
		}
		else if (parser->states[0].token == XS_TOKEN_LEFT_PARENTHESIS) {
			f->u1 = 0;
			f->n0 = NULL;
			if (parser->root->description && (parser->root->description->token == XS_TOKEN_ACCESS)) {
				txAccessNode* access = (txAccessNode*)parser->root;
				f->n0 = (txNode*)access;
				if (access->symbol == parser->evalSymbol) {
					parser->flags |= mxEvalFlag;
				}
				else if (parser->flags & mxCFlag) {
					if (access->symbol == parser->NativeSymbol)
						f->u1 = mxNativeConstructorFlag;
					else if (access->symbol == parser->nativeSymbol) {
						f->u1 = mxNativeFunctionFlag;
						parser->flags |= mxNativeFlag;
					}
				}
			}
			fxParserCall(parser, K_PARAMETERS, parser->states[0].line);
			P_RESUME(4);
		}
		else if (parser->states[0].token == XS_TOKEN_TEMPLATE) {
			if (f->b0)
				fxReportParserError(parser, parser->states[0].line, "invalid template");
			fxPushStringNode(parser, parser->states[0].stringLength, parser->states[0].string, f->i1);
			fxPushRawNode(parser, parser->states[0].rawLength, parser->states[0].raw, f->i1);
			fxPushNodeStruct(parser, 2, XS_TOKEN_TEMPLATE_MIDDLE, f->i1);
			fxGetNextToken(parser);
			fxPushNodeList(parser, 1);
			fxPushNodeStruct(parser, 2, XS_TOKEN_TEMPLATE, f->i1);
			f->pc = 2;
			return;
		}
		else if (parser->states[0].token == XS_TOKEN_TEMPLATE_HEAD) {
			if (f->b0)
				fxReportParserError(parser, parser->states[0].line, "invalid template");
			fxParserCall(parser, K_TEMPLATE_EXPR, parser->states[0].line);
			P_RESUME(5);
		}
		else if (parser->states[0].token == XS_TOKEN_CHAIN) {
			fxGetNextToken(parser);
			f->b0 = 1;
			if (parser->states[0].token == XS_TOKEN_IDENTIFIER) {
				fxPushNodeStruct(parser, 1, XS_TOKEN_OPTION, f->i1);
				fxPushSymbol(parser, parser->states[0].symbol);
				fxPushNodeStruct(parser, 2, XS_TOKEN_MEMBER, f->i1);
				fxGetNextToken(parser);
			}
			else if (parser->states[0].token == XS_TOKEN_PRIVATE_IDENTIFIER) {
				fxPushNodeStruct(parser, 1, XS_TOKEN_OPTION, f->i1);
				fxPushSymbol(parser, parser->states[0].symbol);
				fxSwapNodes(parser);
				fxPushNodeStruct(parser, 2, XS_TOKEN_PRIVATE_MEMBER, f->i1);
				fxGetNextToken(parser);
			}
			else if (parser->states[0].token == XS_TOKEN_LEFT_BRACKET) {
				fxPushNodeStruct(parser, 1, XS_TOKEN_OPTION, f->i1);
				fxGetNextToken(parser);
				fxParserCall(parser, K_COMMA, parser->states[0].line);
				P_RESUME(6);
			}
			else if (parser->states[0].token == XS_TOKEN_LEFT_PARENTHESIS) {
				fxPushNodeStruct(parser, 1, XS_TOKEN_OPTION, f->i1);
				fxParserCall(parser, K_PARAMETERS, parser->states[0].line);
				P_RESUME(7);
			}
			else
				fxReportParserError(parser, parser->states[0].line, "invalid ?.");
			f->pc = 2;
			return;
		}
		else {
			if (f->b0)
				fxPushNodeStruct(parser, 1, XS_TOKEN_CHAIN, f->i0);
			break;
		}
	case 3:
		fxPushNodeStruct(parser, 2, XS_TOKEN_MEMBER_AT, f->i1);
		fxMatchToken(parser, XS_TOKEN_RIGHT_BRACKET);
		f->pc = 2;
		return;
	case 4:
		if (f->u1) {
			txParamsNode* params = (txParamsNode*)parser->root;
			txAccessNode* access = (txAccessNode*)f->n0;
			txStringNode* param;
			if (params->items->length == 0)
				fxReportParserError(parser, f->i1, "%s: no argument", access->symbol->string);
			if (params->items->length > 1)
				fxReportParserError(parser, f->i1, "%s: too many arguments", access->symbol->string);
			param = (txStringNode*)(params->items->first);
			if (param->description->token != XS_TOKEN_STRING)
				fxReportParserError(parser, f->i1, "%s: argument is no string literal", access->symbol->string);
			fxPopNode(parser);
			fxPopNode(parser);
			if (f->u1 & mxNativeFunctionFlag) {
				fxPushNULL(parser);
				fxPushNodeList(parser, 0);
				fxPushNodeStruct(parser, 1, XS_TOKEN_PARAMS_BINDING, f->i1);
				fxPushStringNode(parser, param->length, param->value, f->i1);
				fxPushNodeStruct(parser, 3, XS_TOKEN_HOST, f->i1);
			}
			else {
				fxPushNULL(parser);
				fxPushNULL(parser);
				fxPushNULL(parser);
				fxPushStringNode(parser, param->length, param->value, f->i1);
				fxPushNodeStruct(parser, 3, XS_TOKEN_HOST, f->i1);
				fxPushNodeList(parser, 0);
				fxPushNULL(parser);
				fxPushNULL(parser);
				fxPushNULL(parser);
				fxPushNodeList(parser, 0);
				fxPushNodeStruct(parser, 1, XS_TOKEN_PARAMS_BINDING, f->i1);
				fxPushNodeStruct(parser, 0, XS_TOKEN_UNDEFINED, f->i1);
				fxPushNodeStruct(parser, 1, XS_TOKEN_STATEMENT, f->i1);
				fxPushNodeStruct(parser, 1, XS_TOKEN_BODY, f->i1);
				fxPushNodeStruct(parser, 3, XS_TOKEN_FUNCTION, f->i1);
				parser->root->flags = mxStrictFlag | mxBaseFlag | mxMethodFlag | mxTargetFlag;
				fxPushNodeStruct(parser, 6, XS_TOKEN_CLASS, f->i1);
			}
		}
		else
			fxPushNodeStruct(parser, 2, XS_TOKEN_CALL, f->i1);
		f->pc = 2;
		return;
	case 5:
		fxPushNodeStruct(parser, 2, XS_TOKEN_TEMPLATE, f->i1);
		f->pc = 2;
		return;
	case 6:
		fxPushNodeStruct(parser, 2, XS_TOKEN_MEMBER_AT, f->i1);
		fxMatchToken(parser, XS_TOKEN_RIGHT_BRACKET);
		f->pc = 2;
		return;
	case 7:
		fxPushNodeStruct(parser, 2, XS_TOKEN_CALL, f->i1);
		f->pc = 2;
		return;
	}
	fxParserReturn(parser);
}

/* xs_no_recursion: class expression. Full stock member loop converted to
   a resumable state machine (methods/fields/static blocks are preserved
   verbatim; re-entrant members go through the trampoline). */
void fxClassExpressionStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->u1 = f->u2; /* flag argument */
		f->i0 = f->line; /* theLine */
		f->b0 = 0; /* heritageFlag */
		f->n0 = NULL; /* constructor */
		f->i1 = 0; /* aCount */
		f->i2 = 0; /* constructorInitCount */
		f->i3 = 0; /* instanceInitCount */
		f->u0 = mxSuperFlag; /* constructorFlags */
		f->u2 = parser->flags;
		f->s0 = NULL; /* *theSymbol out */
		parser->flags |= mxStrictFlag;
		fxMatchToken(parser, XS_TOKEN_CLASS);
		if (parser->states[0].token == XS_TOKEN_IDENTIFIER) {
			f->s0 = parser->states[0].symbol;
			fxPushSymbol(parser, f->s0);
			fxGetNextToken(parser);
		}
		else
			fxPushNULL(parser);
		if (parser->states[0].token != XS_TOKEN_EXTENDS) {
			f->pc = 11;
			return;
		}
		fxMatchToken(parser, XS_TOKEN_EXTENDS);
		fxParserCall(parser, K_CALL_CHAIN, f->i0);
		P_RESUME(1);
	case 1:
		fxCheckArrowFunction(parser, 1);
		fxCheckNativeConstructor(parser);
		f->u2 |= parser->flags & mxAwaitingFlag;
		f->u0 |= mxDerivedFlag;
		if (parser->root->description->token == XS_TOKEN_HOST)
			f->u0 |= mxHostFlag;
		f->b0 = 1;
		f->pc = 11;
		return;
	case 11:
		if (!f->b0) {
			/* xs_no_recursion: stock reaches the host/base fallback only when
			   no heritage was parsed; this case also runs after the heritage
			   sub-parse (b0==1), which must not push the base NULL. */
			if (parser->states[0].token == XS_TOKEN_HOST) {
				fxGetNextToken(parser);
				fxPushNULL(parser);
				fxPushNULL(parser);
				if (parser->states[0].token == XS_TOKEN_STRING) {
					fxPushStringNode(parser, parser->states[0].stringLength, parser->states[0].string, f->i0);
					fxGetNextToken(parser);
				}
				else {
					fxReportParserError(parser, parser->states[0].line, "invalid host class");
					fxPushNULL(parser);
				}
				fxPushNodeStruct(parser, 3, XS_TOKEN_HOST, f->i0);
				f->u0 |= mxBaseFlag | mxHostFlag;
			}
			else {
				fxPushNULL(parser);
				f->u0 |= mxBaseFlag;
			}
		}
		if (parser->states[0].token != XS_TOKEN_LEFT_BRACE) {
			f->pc = 30; /* finish */
			return;
		}
		fxMatchToken(parser, XS_TOKEN_LEFT_BRACE);
		f->pc = 12;
		return;
	case 12:
		/* member loop head */
		while (parser->states[0].token == XS_TOKEN_SEMICOLON)
			fxGetNextToken(parser);
		if (parser->states[0].token == XS_TOKEN_RIGHT_BRACE) {
			f->pc = 30;
			return;
		}
		f->b1 = 0; /* aStaticFlag */
		f->i4 = parser->states[0].line; /* aPropertyLine */
		f->s1 = C_NULL;
		f->t0 = XS_NO_TOKEN;
		f->t1 = XS_NO_TOKEN;
		f->t2 = XS_NO_TOKEN;
		f->u1 = 0; /* flag */
		if ((parser->states[0].token == XS_TOKEN_STATIC) && (!parser->states[0].escaped)) {
			fxGetNextToken(parser);
			if ((parser->states[0].token == XS_TOKEN_ASSIGN) || (parser->states[0].token == XS_TOKEN_SEMICOLON)) {
				fxPushSymbol(parser, parser->staticSymbol);
				f->t1 = XS_TOKEN_PROPERTY;
				f->pc = 20; /* field */
				return;
			}
			if (parser->states[0].token == XS_TOKEN_LEFT_BRACE) {
				/* static block */
				f->u1 = parser->flags;
				parser->flags = (f->u1 & (mxParserFlags | mxStrictFlag)) | mxSuperFlag | mxTargetFlag | mxFieldFlag | mxAsyncFlag;
				fxCheckParserStack(parser, f->i4);
				fxGetNextToken(parser);
				fxParserCall(parser, K_STATEMENTS, f->i4);
				P_RESUME(13);
			}
			f->b1 = 1;
		}
		f->pc = 14;
		return;
	case 13:
		fxMatchToken(parser, XS_TOKEN_RIGHT_BRACE);
		fxPushNodeStruct(parser, 1, XS_TOKEN_BODY, f->i4);
		if (parser->flags & mxArgumentsFlag)
			fxReportParserError(parser, parser->states[0].line, "invalid arguments");
		if (parser->flags & mxAwaitingFlag)
			fxReportParserError(parser, parser->states[0].line, "invalid await");
		parser->flags = f->u1;
		parser->root->flags |= mxStaticFlag;
		f->i2++;
		f->i1++;
		f->pc = 12;
		return;
	case 14:
		f->u1 = 0;
		fxParserCall(parser, K_PROPERTY_NAME, f->i4);
		P_RESUME(15);
	case 15:
		f->s1 = parser->outSymbol;
		f->t0 = parser->outToken0;
		f->t1 = parser->outToken1;
		f->t2 = parser->outToken2;
		f->u1 = parser->outFlags;
		if ((f->b1 == 0) && (f->s1 == parser->constructorSymbol)) {
			fxPopNode(parser); /* symbol */
			if (f->n0 || (f->t2 == XS_TOKEN_GENERATOR) || (f->t2 == XS_TOKEN_GETTER) || (f->t2 == XS_TOKEN_SETTER) || (f->u1 & mxAsyncFlag))
				fxReportParserError(parser, parser->states[0].line, "invalid constructor");
			fxParserCallTokenFlag(parser, K_FUNCTION_EXPR, f->i4, XS_NO_TOKEN, f->u0);
			P_RESUME(16);
		}
		if (parser->states[0].token != XS_TOKEN_LEFT_PARENTHESIS) {
			f->pc = 20; /* field */
			return;
		}
		if ((f->t1 == XS_TOKEN_PRIVATE_PROPERTY) && (f->s1 == parser->privateConstructorSymbol))
			fxReportParserError(parser, parser->states[0].line, "invalid method: #constructor");
		if (f->b1 && (f->s1 == parser->prototypeSymbol))
			fxReportParserError(parser, parser->states[0].line, "invalid static method: prototype");
		if (f->b1)
			f->u1 |= mxStaticFlag;
		if (f->t2 == XS_TOKEN_GETTER)
			f->u1 |= mxGetterFlag;
		else if (f->t2 == XS_TOKEN_SETTER)
			f->u1 |= mxSetterFlag;
		else
			f->u1 |= mxMethodFlag;
		if (f->t2 == XS_TOKEN_GENERATOR)
			fxParserCallTokenFlag(parser, K_GENERATOR_EXPR, f->i4, XS_NO_TOKEN, mxSuperFlag | f->u1);
		else
			fxParserCallTokenFlag(parser, K_FUNCTION_EXPR, f->i4, XS_NO_TOKEN, mxSuperFlag | f->u1);
		P_RESUME(17);
	case 16:
		f->n0 = fxPopNode(parser);
		f->pc = 12;
		return;
	case 17:
		fxPushNodeStruct(parser, 2, f->t1, f->i4);
		parser->root->flags |= f->u1 & (mxStaticFlag | mxGetterFlag | mxSetterFlag | mxMethodFlag);
		if (f->t1 == XS_TOKEN_PRIVATE_PROPERTY) {
			if (f->b1)
				f->i2++;
			else
				f->i3++;
		}
		f->i1++;
		f->pc = 21; /* member tail */
		return;
	case 20: /* field */
		if (f->t1 != XS_TOKEN_PROPERTY) {
			if ((f->t1 == XS_TOKEN_PRIVATE_PROPERTY) && (f->s1 == parser->privateConstructorSymbol))
				fxReportParserError(parser, parser->states[0].line, "invalid field: #constructor");
			if (f->s1 == parser->constructorSymbol)
				fxReportParserError(parser, parser->states[0].line, "invalid field: constructor");
			if (f->s1 == parser->prototypeSymbol)
				fxReportParserError(parser, parser->states[0].line, "invalid field: prototype");
		}
		if (parser->states[0].token == XS_TOKEN_ASSIGN) {
			f->u1 = parser->flags;
			parser->flags = (f->u1 & (mxParserFlags | mxStrictFlag)) | mxSuperFlag | mxTargetFlag | mxFieldFlag;
			fxGetNextToken(parser);
			fxParserCall(parser, K_ASSIGN, f->i4);
			P_RESUME(22);
		}
		fxPushNodeStruct(parser, 0, XS_TOKEN_UNDEFINED, f->i0);
		f->pc = 23;
		return;
	case 22:
		if (parser->flags & mxArgumentsFlag)
			fxReportParserError(parser, parser->states[0].line, "invalid arguments");
		parser->flags = f->u1;
		f->pc = 23;
		return;
	case 23:
		fxPushNodeStruct(parser, 2, f->t1, f->i4);
		if (f->b1) {
			parser->root->flags |= mxStaticFlag;
			f->i2++;
		}
		else
			f->i3++;
		fxSemicolon(parser);
		f->i1++;
		f->pc = 21;
		return;
	case 21: /* member tail: comma or loop */
		if (parser->states[0].token == XS_TOKEN_RIGHT_BRACE) {
			f->pc = 30;
			return;
		}
		f->pc = 12;
		return;
	case 30: /* finish: build init lists and push CLASS node */
		{
			txNodeList* itemsList;
			txNodeList* constructorInitList = C_NULL;
			txNode** constructorInitAddress = C_NULL;
			txNodeList* instanceInitList = C_NULL;
			txNode** instanceInitAddress = C_NULL;
			txNode** address;
			txNode* item;
			fxMatchToken(parser, XS_TOKEN_RIGHT_BRACE);
			fxPushNodeList(parser, f->i1);
			itemsList = (txNodeList*)(parser->root);
			if (f->i2 || f->i3) {
				if (f->i2) {
					fxPushNULL(parser);
					fxPushNodeList(parser, 0);
					fxPushNodeStruct(parser, 1, XS_TOKEN_PARAMS_BINDING, f->i0);
					fxPushNodeList(parser, 0);
					constructorInitList = (txNodeList*)(parser->root);
					constructorInitList->length = f->i2;
					constructorInitAddress = &(((txNodeList*)(parser->root))->first);
					fxPushNodeStruct(parser, 1, XS_TOKEN_STATEMENTS, f->i0);
					fxPushNodeStruct(parser, 1, XS_TOKEN_BODY, f->i0);
					fxPushNodeStruct(parser, 3, XS_TOKEN_FUNCTION, f->i0);
					parser->root->flags = mxStrictFlag | mxSuperFlag | mxFieldFlag;
				}
				else
					fxPushNULL(parser);
				if (f->i3) {
					fxPushNULL(parser);
					fxPushNodeList(parser, 0);
					fxPushNodeStruct(parser, 1, XS_TOKEN_PARAMS_BINDING, f->i0);
					fxPushNodeList(parser, 0);
					instanceInitList = (txNodeList*)(parser->root);
					instanceInitList->length = f->i3;
					instanceInitAddress = &(((txNodeList*)(parser->root))->first);
					fxPushNodeStruct(parser, 1, XS_TOKEN_STATEMENTS, f->i0);
					fxPushNodeStruct(parser, 1, XS_TOKEN_BODY, f->i0);
					fxPushNodeStruct(parser, 3, XS_TOKEN_FUNCTION, f->i0);
					parser->root->flags = mxStrictFlag | mxSuperFlag | mxFieldFlag;
				}
				else
					fxPushNULL(parser);
				(void)instanceInitList;
				address = &(itemsList->first);
				while ((item = *address)) {
					if (item->flags & (mxMethodFlag | mxGetterFlag | mxSetterFlag)) {
						if (item->description->token == XS_TOKEN_PRIVATE_PROPERTY) {
							txFieldNode* field = fxFieldNodeNew(parser, XS_TOKEN_FIELD);
							field->item = item;
							if (item->flags & mxStaticFlag) {
								*constructorInitAddress = (txNode*)field;
								constructorInitAddress = &field->next;
							}
							else {
								*instanceInitAddress = (txNode*)field;
								instanceInitAddress = &field->next;
							}
						}
					}
					address = &(item->next);
				}
				address = &(itemsList->first);
				while ((item = *address)) {
					if (item->description->token == XS_TOKEN_BODY) {
						*address = item->next;
						item->next = C_NULL;
						itemsList->length--;
						*constructorInitAddress = (txNode*)item;
						constructorInitAddress = &item->next;
					}
					else if (item->flags & (mxMethodFlag | mxGetterFlag | mxSetterFlag)) {
						address = &(item->next);
					}
					else {
						txFieldNode* field = fxFieldNodeNew(parser, XS_TOKEN_FIELD);
						field->item = item;
						if (item->description->token == XS_TOKEN_PROPERTY) {
							field->value = ((txPropertyNode*)item)->value;
							((txPropertyNode*)item)->value = C_NULL;
						}
						else if (item->description->token == XS_TOKEN_PROPERTY_AT) {
							field->value = ((txPropertyAtNode*)item)->value;
							((txPropertyAtNode*)item)->value = C_NULL;
						}
						else {
							field->value = ((txPrivatePropertyNode*)item)->value;
							((txPrivatePropertyNode*)item)->value = C_NULL;
						}
						if (item->flags & mxStaticFlag) {
							*constructorInitAddress = (txNode*)field;
							constructorInitAddress = &field->next;
						}
						else {
							*instanceInitAddress = (txNode*)field;
							instanceInitAddress = &field->next;
						}
						address = &(item->next);
					}
				}
			}
			else {
				fxPushNULL(parser);
				fxPushNULL(parser);
			}
			if (f->n0) {
				fxPushNode(parser, f->n0);
			}
			else {
				if (f->b0) {
					fxPushNULL(parser);
					fxPushSymbol(parser, parser->argsSymbol);
					fxPushNULL(parser);
					fxPushNodeStruct(parser, 2, XS_TOKEN_ARG, f->i0);
					fxPushNodeStruct(parser, 1, XS_TOKEN_REST_BINDING, f->i0);
					fxPushNodeList(parser, 1);
					fxPushNodeStruct(parser, 1, XS_TOKEN_PARAMS_BINDING, f->i0);
					fxPushSymbol(parser, parser->argsSymbol);
					fxPushNodeStruct(parser, 1, XS_TOKEN_ACCESS, f->i0);
					fxPushNodeStruct(parser, 1, XS_TOKEN_SPREAD, f->i0);
					fxPushNodeList(parser, 1);
					fxPushNodeStruct(parser, 1, XS_TOKEN_PARAMS, f->i0);
					parser->root->flags |= mxSpreadFlag;
					fxPushNodeStruct(parser, 1, XS_TOKEN_SUPER, f->i0);
					fxPushNodeStruct(parser, 1, XS_TOKEN_STATEMENT, f->i0);
					fxPushNodeStruct(parser, 1, XS_TOKEN_BODY, f->i0);
					fxPushNodeStruct(parser, 3, XS_TOKEN_FUNCTION, f->i0);
					parser->root->flags = mxStrictFlag | mxDerivedFlag | mxMethodFlag | mxTargetFlag | mxSuperFlag;
				}
				else {
					fxPushNULL(parser);
					fxPushNodeList(parser, 0);
					fxPushNodeStruct(parser, 1, XS_TOKEN_PARAMS_BINDING, f->i0);
					fxPushNodeStruct(parser, 0, XS_TOKEN_UNDEFINED, f->i0);
					fxPushNodeStruct(parser, 1, XS_TOKEN_STATEMENT, f->i0);
					fxPushNodeStruct(parser, 1, XS_TOKEN_BODY, f->i0);
					fxPushNodeStruct(parser, 3, XS_TOKEN_FUNCTION, f->i0);
					parser->root->flags = mxStrictFlag | mxBaseFlag | mxMethodFlag | mxTargetFlag;
				}
			}
			fxPushNodeStruct(parser, 6, XS_TOKEN_CLASS, f->i0);
			parser->flags = f->u2 | (parser->flags & mxArgumentsFlag);
		}
		break;
	}
	/* publish the class name for callers like fxStatement/fxExportDeclaration:
	   fxParserReturn copies s0 into parser->outSymbol when this frame pops. */
	fxParserReturn(parser);
}
void fxJSXAttributeValueStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->i0 = parser->states[0].line;
		fxGetNextTokenJSXAttribute(parser);
		if (parser->states[0].token == XS_TOKEN_STRING) {
			fxPushStringNode(parser, parser->states[0].stringLength, parser->states[0].string, f->i0);
			fxGetNextToken(parser);
			break;
		}
		if (parser->states[0].token == XS_TOKEN_LEFT_BRACE) {
			fxGetNextToken(parser);
			if (parser->states[0].token == XS_TOKEN_RIGHT_BRACE) {
				fxGetNextToken(parser);
				fxReportParserError(parser, parser->states[0].line, "missing expression");
				fxPushNodeStruct(parser, 0, XS_TOKEN_UNDEFINED, f->i0);
				break;
			}
			fxParserCall(parser, K_ASSIGN, f->i0);
			P_RESUME(1);
		}
		fxReportParserError(parser, parser->states[0].line, "invalid %s", gxTokenNames[parser->states[0].token]);
		fxPushNodeStruct(parser, 0, XS_TOKEN_UNDEFINED, f->i0);
		break;
	case 1:
		if (parser->states[0].token == XS_TOKEN_RIGHT_BRACE)
			fxGetNextToken(parser);
		else
			fxReportParserError(parser, parser->states[0].line, "missing }");
		break;
	}
	fxParserReturn(parser);
}

/* xs_no_recursion: JSX element - child elements recurse through the
   trampoline; the element loop itself is iterative on the frame. */
void fxJSXElementStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	switch (f->pc) {
	case 0:
		f->b0 = 0; /* closed */
		f->i0 = parser->states[0].line;
		f->n0 = NULL; /* name */
		f->i1 = parser->nodeCount;
		f->i2 = 0; /* propertyCount */
		fxCheckParserStack(parser, f->i0);
		fxPushSymbol(parser, parser->__jsx__Symbol);
		fxPushNodeStruct(parser, 1, XS_TOKEN_ACCESS, parser->states[0].line);
		if (parser->states[0].token == XS_TOKEN_IDENTIFIER) {
			fxJSXElementName(parser);
			f->n0 = parser->root;
		}
		else {
			fxReportParserError(parser, parser->states[0].line, "missing identifier");
			fxPushNULL(parser);
		}
		f->pc = 1;
		return;
	case 1:
		for (;;) {
			if (parser->states[0].token == XS_TOKEN_MORE) {
				f->pc = 2;
				return;
			}
			if (parser->states[0].token == XS_TOKEN_DIVIDE) {
				fxGetNextToken(parser);
				if (parser->states[0].token != XS_TOKEN_MORE)
					fxReportParserError(parser, parser->states[0].line, "missing >");
				f->b0 = 1;
				f->pc = 2;
				return;
			}
			if (gxTokenFlags[parser->states[0].token] & XS_TOKEN_IDENTIFIER_NAME) {
				fxJSXAttributeName(parser);
				if (parser->states[0].token == XS_TOKEN_ASSIGN) {
					fxParserCall(parser, K_JSX_ATTRIBUTE_VALUE, parser->states[0].line);
					P_RESUME(3);
				}
				else
					fxPushNodeStruct(parser, 0, XS_TOKEN_TRUE, parser->states[0].line);
				fxPushNodeStruct(parser, 2, XS_TOKEN_PROPERTY, parser->states[0].line);
				f->i2++;
				continue;
			}
			if (parser->states[0].token == XS_TOKEN_LEFT_BRACE) {
				fxGetNextToken(parser);
				if (parser->states[0].token == XS_TOKEN_SPREAD) {
					fxGetNextToken(parser);
					fxParserCall(parser, K_ASSIGN, parser->states[0].line);
					P_RESUME(4);
				}
				fxReportParserError(parser, parser->states[0].line, "invalid %s", gxTokenNames[parser->states[0].token]);
				continue;
			}
			fxReportParserError(parser, parser->states[0].line, "invalid %s", gxTokenNames[parser->states[0].token]);
			break;
		}
		f->pc = 2;
		return;
	case 3:
		fxPushNodeStruct(parser, 2, XS_TOKEN_PROPERTY, parser->states[0].line);
		f->i2++;
		f->pc = 1;
		return;
	case 4:
		if (parser->states[0].token == XS_TOKEN_RIGHT_BRACE)
			fxGetNextToken(parser);
		else
			fxReportParserError(parser, parser->states[0].line, "missing }");
		fxPushNodeStruct(parser, 1, XS_TOKEN_SPREAD, parser->states[0].line);
		f->i2++;
		f->pc = 1;
		return;
	case 2:
		if (f->i2 > 0) {
			fxPushNodeList(parser, f->i2);
			fxPushNodeStruct(parser, 1, XS_TOKEN_OBJECT, parser->states[0].line);
		}
		else
			fxPushNodeStruct(parser, 0, XS_TOKEN_NULL, parser->states[0].line);
		if (f->b0) {
			fxPushNodeList(parser, parser->nodeCount - f->i1 - 1);
			fxPushNodeStruct(parser, 1, XS_TOKEN_PARAMS, f->i0);
			fxPushNodeStruct(parser, 2, XS_TOKEN_CALL, f->i0);
			break;
		}
		f->pc = 5;
		return;
	case 5:
		for (;;) {
			fxGetNextTokenJSXChild(parser);
			if (parser->states[0].stringLength)
				fxPushStringNode(parser, parser->states[0].stringLength, parser->states[0].string, parser->states[0].line);
			if (parser->states[0].token == XS_TOKEN_LEFT_BRACE) {
				fxGetNextToken(parser);
				if (parser->states[0].token == XS_TOKEN_RIGHT_BRACE) {
					fxGetNextToken(parser);
					continue;
				}
				fxParserCall(parser, K_ASSIGN, parser->states[0].line);
				P_RESUME(6);
			}
			else if (parser->states[0].token == XS_TOKEN_LESS) {
				fxGetNextToken(parser);
				if (parser->states[0].token == XS_TOKEN_DIVIDE) {
					fxGetNextToken(parser);
					if (parser->states[0].token == XS_TOKEN_IDENTIFIER) {
						fxJSXElementName(parser);
						if (!fxJSXMatch(parser, f->n0, parser->root)) {
							fxReportParserError(parser, parser->states[0].line, "invalid element");
							fxPushNULL(parser);
						}
					}
					else {
						fxReportParserError(parser, parser->states[0].line, "missing identifier");
						fxPushNULL(parser);
					}
					if (parser->states[0].token != XS_TOKEN_MORE)
						fxReportParserError(parser, parser->states[0].line, "missing >");
					fxPopNode(parser);
					f->pc = 7;
					return;
				}
				fxParserCall(parser, K_JSX_ELEMENT, parser->states[0].line);
				P_RESUME(8);
			}
			else {
				fxReportParserError(parser, parser->states[0].line, "invalid %s", gxTokenNames[parser->states[0].token]);
				f->pc = 7;
				return;
			}
		}
	case 6:
		if (parser->states[0].token != XS_TOKEN_RIGHT_BRACE)
			fxReportParserError(parser, parser->states[0].line, "missing }");
		f->pc = 5;
		return;
	case 8:
		f->pc = 5;
		return;
	case 7:
		fxPushNodeList(parser, parser->nodeCount - f->i1 - 1);
		fxPushNodeStruct(parser, 1, XS_TOKEN_PARAMS, f->i0);
		fxPushNodeStruct(parser, 2, XS_TOKEN_CALL, f->i0);
		break;
	}
	fxParserReturn(parser);
}


static txSymbol* fxJSXName(txParser* parser, txSymbol* before, txSymbol* after)
{
	txSize beforeLength = before->length;
	txSize afterLength = after->length;
	txSize length = beforeLength + 1 + afterLength + 1;
	txString string = fxNewParserChunk(parser, length);
	snprintf(string, length, "%s-%s", before->string, after->string);
	return fxNewParserSymbol(parser, string);
}

/* xs_no_recursion: native helper/token-pusher bodies restored from the
   stock 9.5.0 source — the trampoline conversion calls these verbatim;
   none of them recurse into the converted grammar routines. */

txBoolean fxIsKeyword(txParser* parser, txSymbol* keyword)
{
	txBoolean result = ((parser->states[0].token == XS_TOKEN_IDENTIFIER) && (parser->states[0].symbol == keyword)) ? 1 : 0;
	if (result) {
		if (parser->states[0].escaped)
			fxReportParserError(parser, parser->states[0].line, "escaped keyword");
	}
	return result;
}

txBoolean fxIsToken(txParser* parser, txToken theToken)
{
	txBoolean result = (parser->states[0].token == theToken) ? 1 : 0;
	if (result) {
		if (parser->states[0].escaped)
			fxReportParserError(parser, parser->states[0].line, "escaped keyword");
	}
	return result;
}

void fxMatchToken(txParser* parser, txToken theToken)
{
	if (parser->states[0].token == theToken) {
		if (parser->states[0].escaped)
			fxReportParserError(parser, parser->states[0].line, "escaped keyword");
		fxGetNextToken(parser);
	}
	else
		fxReportParserError(parser, parser->states[0].line, "missing %s", gxTokenNames[theToken]);
}

txNode* fxPopNode(txParser* parser)
{
	txNode* node = parser->root;
	parser->root = node->next;
	node->next = NULL;
	parser->nodeCount--;
	return node;
}

void fxPushNode(txParser* parser, txNode* node)
{
	node->next = parser->root;
	parser->root = (txNode*)node;
	parser->nodeCount++;
}

void fxPushBigIntNode(txParser* parser, txBigInt* value, txInteger line)
{
	txBigIntNode* node = fxNewParserChunk(parser, sizeof(txBigIntNode));
	node->description = &gxTokenDescriptions[XS_TOKEN_BIGINT];
	node->path = parser->path;
	node->line = line;
	node->flags = 0;
	node->value = *value;
	fxPushNode(parser, (txNode*)node);
}

void fxPushIndexNode(txParser* parser, txIndex value, txInteger line)
{
	if (((txInteger)value) >= 0)
		fxPushIntegerNode(parser, (txInteger)value, line);
	else
		fxPushNumberNode(parser, value, line);
}

void fxPushIntegerNode(txParser* parser, txInteger value, txInteger line)
{
	txIntegerNode* node = fxNewParserChunk(parser, sizeof(txIntegerNode));
	node->description = &gxTokenDescriptions[XS_TOKEN_INTEGER];
	node->path = parser->path;
	node->line = line;
	node->flags = 0;
	node->value = value;
	fxPushNode(parser, (txNode*)node);
}

void fxPushNodeStruct(txParser* parser, txInteger count, txToken token, txInteger line)
{
	const txNodeDescription* description = &gxTokenDescriptions[token];
	txNode* node;
	if ((count > parser->nodeCount) || ((sizeof(txNode) + (count * sizeof(txNode*))) > (size_t)(description->size))) {
#ifdef XS_NR_PUMP_TRACE
		c_fprintf(stderr, "PUSHFAIL token=%s count=%d nodeCount=%d off=%ld\n", gxTokenNames[token], (int)count, (int)parser->nodeCount, parser->stream ? (long)((txStringCStream*)parser->stream)->offset : -1L);
#endif
		fxReportParserError(parser, parser->states[0].line, "invalid %s", gxTokenNames[token]);
	}
    node = fxNewParserChunkClear(parser, description->size);
	node->description = description;
	node->flags |= parser->flags & (mxStrictFlag | mxGeneratorFlag | mxAsyncFlag);
	node->path = parser->path;
	node->line = line;
    parser->nodeCount -= count;
	if (count) {
		txNode** dst = (txNode**)&node[1];
		txNode* src = parser->root;
		while (count) {
			txNode* next = src->next;
			src->next = NULL;
			count--;
			if (src->description)
				dst[count] = src;
			else if (src->path)
				dst[count] = (txNode*)(src->path);
			src = next;
		}
		parser->root = src;
	}
	fxPushNode(parser, node);
}

void fxPushNodeList(txParser* parser, txInteger count)
{
	txNodeList* list = fxNewParserChunk(parser, sizeof(txNodeList));
	txNode* previous = NULL;
	txNode* current = parser->root;
	txNode* next;
    parser->nodeCount -= count;
	list->length = count;
	while (count) {
		next = current->next;
		current->next = previous;
		previous = current;
		current = next;
		count--;
	}
	parser->root = current;
	list->description = &gxTokenDescriptions[XS_NO_TOKEN];
	list->first = previous;
	fxPushNode(parser, (txNode*)list);
}

void fxPushNULL(txParser* parser)
{
    txNodeLink* node = fxNewParserChunkClear(parser, sizeof(txNodeLink));
	fxPushNode(parser, (txNode*)node);
}

void fxPushNumberNode(txParser* parser, txNumber value, txInteger line)
{
	txNumberNode* node = fxNewParserChunk(parser, sizeof(txNumberNode));
	node->description = &gxTokenDescriptions[XS_TOKEN_NUMBER];
	node->path = parser->path;
	node->line = line;
	node->flags = 0;
	node->value = value;
	fxPushNode(parser, (txNode*)node);
}

void fxPushRawNode(txParser* parser, txInteger length, txString value, txInteger line)
{
	txStringNode* node = fxNewParserChunk(parser, sizeof(txStringNode));
	node->description = &gxTokenDescriptions[XS_TOKEN_STRING];
	node->path = parser->path;
	node->line = line;
	node->flags = 0;
	node->length = length;
	node->value = value;
	fxPushNode(parser, (txNode*)node);
}

void fxPushStringNode(txParser* parser, txInteger length, txString value, txInteger line)
{
	txStringNode* node = fxNewParserChunk(parser, sizeof(txStringNode));
	node->description = &gxTokenDescriptions[XS_TOKEN_STRING];
	node->path = parser->path;
	node->line = line;
	node->flags = parser->states[0].escaped;
	node->length = length;
	node->value = value;
	fxPushNode(parser, (txNode*)node);
}

void fxPushSymbol(txParser* parser, txSymbol* symbol)
{
    txNodeLink* node = fxNewParserChunkClear(parser, sizeof(txNodeLink));
	node->symbol = symbol;	
	fxPushNode(parser, (txNode*)node);
}

void fxSwapNodes(txParser* parser)
{
	txNode* previous = parser->root;
	txNode* current = previous->next;
	previous->next = current->next;
	current->next = previous;
	parser->root = current;
}

void fxSemicolon(txParser* parser)
{
#ifdef XS_NR_PUMP_TRACE
	c_fprintf(stderr, "SEMI tok=%d (%s) crlf=%d\n", (int)parser->states[0].token, gxTokenNames[parser->states[0].token], (int)parser->states[0].crlf);
#endif
	if ((parser->states[0].crlf) || (gxTokenFlags[parser->states[0].token] & XS_TOKEN_END_STATEMENT)) {
		if (parser->states[0].token == XS_TOKEN_SEMICOLON)
			fxGetNextToken(parser);
	}
	else
		fxReportParserError(parser, parser->states[0].line, "missing ;");
}

txNode* fxParametersBindingFromExpressions(txParser* parser, txNode* theNode)
{
	txNodeList* list = ((txParamsNode*)theNode)->items;
	txNode** address = &(list->first);
	txNode* item;
	txBindingNode* binding;
	while ((item = *address)) {
		txToken aToken = (item && item->description) ? item->description->token : XS_NO_TOKEN;
		if (aToken == XS_TOKEN_SPREAD) {
			binding = (txBindingNode*)fxRestBindingFromExpression(parser, item, XS_TOKEN_ARG, 0);
			if (!binding)
				return NULL;
			parser->flags |= mxNotSimpleParametersFlag;
			break;
		}
		binding = (txBindingNode*)fxBindingFromExpression(parser, item, XS_TOKEN_ARG);
		if (!binding)
			return NULL;
		if (binding->description->token != XS_TOKEN_ARG)
			parser->flags |= mxNotSimpleParametersFlag;
		binding->next = item->next;
		item = *address = (txNode*)binding;
		address = &(item->next);
	}
	theNode->description = &gxTokenDescriptions[XS_TOKEN_PARAMS_BINDING];
	return theNode;
}

void fxCheckArrowFunction(txParser* parser, txInteger count)
{
	txNode* node = parser->root;
	while (count) {
		if (node->flags & mxArrowFlag)
			fxReportParserError(parser, parser->states[0].line, "invalid arrow function");
		count--;
		node = node->next;
	}
}

void fxCheckNativeConstructor(txParser* parser)
{
	txClassNode* root = (txClassNode*)(parser->root);
	if (root->description != &gxTokenDescriptions[XS_TOKEN_CLASS])
		return;
	if (root->symbol != C_NULL)
		return;
	txHostNode* host = (txHostNode*)(root->heritage);
	if (host == C_NULL)
		return;
	if (host->description != &gxTokenDescriptions[XS_TOKEN_HOST])
		return;
	if (root->constructorInit != C_NULL)
		return;
	if (root->instanceInit != C_NULL)
		return;
	txFunctionNode* constructor = (txFunctionNode*)(root->constructor);
	if (constructor->description != &gxTokenDescriptions[XS_TOKEN_FUNCTION])
		return;
	txParamsBindingNode* args = (txParamsBindingNode*)(constructor->params);
	if (args->description != &gxTokenDescriptions[XS_TOKEN_PARAMS_BINDING])
		return;
	if (args->items->length != 0)
		return;
	txBodyNode* body = (txBodyNode*)(constructor->body);
	if (body->description != &gxTokenDescriptions[XS_TOKEN_BODY])
		return;
	txStatementNode* statement = (txStatementNode*)(body->statement);
	if (statement->description != &gxTokenDescriptions[XS_TOKEN_STATEMENT])
		return;
	txNode* undefined = (txNode*)(statement->expression);
	if (undefined->description != &gxTokenDescriptions[XS_TOKEN_UNDEFINED])
		return;
	fxPopNode(parser);
	fxPushNode(parser, (txNode*)host);
}

void fxCheckNativeFunction(txParser* parser)
{
	txFunctionNode* function = (txFunctionNode*)(parser->root);
	txBodyNode* body = (txBodyNode*)(function->body);
	if (body->description != &gxTokenDescriptions[XS_TOKEN_BODY])
		goto bail;
	txStatementNode* statement = (txStatementNode*)(body->statement);
	if ((function->flags & mxDerivedFlag) && (function->flags & mxHostFlag)) {
		txStatementsNode* statements = (txStatementsNode*)(statement);
		if (statements->description != &gxTokenDescriptions[XS_TOKEN_STATEMENTS])
			goto bail;
		if (statements->items->length != 2)
			goto bail;
		statement = (txStatementNode*)(statements->items->first);
		if (statement->description != &gxTokenDescriptions[XS_TOKEN_STATEMENT])
			goto bail;
		txSuperNode* super = (txSuperNode*)(statement->expression);
		if (super->description != &gxTokenDescriptions[XS_TOKEN_SUPER])
			goto bail;
		txParamsNode* params = (txParamsNode*)(super->params);
		if (params->items->length != 0)
			goto bail;
		statement = (txStatementNode*)(statement->next);
	}
	if ((statement->description != &gxTokenDescriptions[XS_TOKEN_RETURN]) && (statement->description != &gxTokenDescriptions[XS_TOKEN_STATEMENT]))
		goto bail;
	txCallNewNode* call = (txCallNewNode*)(statement->expression);
	if (call->description != &gxTokenDescriptions[XS_TOKEN_CALL])
		goto bail;
	txMemberNode* member = (txMemberNode*)(call->reference);
	if (member->description != &gxTokenDescriptions[XS_TOKEN_MEMBER])
		goto bail;
	if (member->symbol != parser->callSymbol)
		goto bail;
	txHostNode* host = (txHostNode*)(member->reference);
	if (host->description != &gxTokenDescriptions[XS_TOKEN_HOST])
		goto bail;
	txParamsNode* params = (txParamsNode*)(call->params);
	if (params->description != &gxTokenDescriptions[XS_TOKEN_PARAMS])
		goto bail;
	txParamsBindingNode* args = (txParamsBindingNode*)(function->params);
	if (args->description != &gxTokenDescriptions[XS_TOKEN_PARAMS_BINDING])
		goto bail;
	if (params->items->length != args->items->length + 1)
		goto bail;
	txNode* arg = args->items->first;
	txNode* param = params->items->first;
	if (param->description != &gxTokenDescriptions[XS_TOKEN_THIS])
		goto bail;
	param = param->next;
	while (arg) {
		txDeclareNode* binding;
		txAccessNode* access;
		if (arg->description == &gxTokenDescriptions[XS_TOKEN_REST_BINDING]) {
			if (param->description != &gxTokenDescriptions[XS_TOKEN_SPREAD])
				goto bail;
			binding = (txDeclareNode*)(((txRestBindingNode*)arg)->binding);
			access = (txAccessNode*)(((txSpreadNode*)param)->expression);
		}
		else {
			binding = (txDeclareNode*)arg;
			access = (txAccessNode*)param;
		}
		if (binding->description != &gxTokenDescriptions[XS_TOKEN_ARG])
			goto bail;
		if (access->description != &gxTokenDescriptions[XS_TOKEN_ACCESS])
			goto bail;
		if (binding->symbol != access->symbol)
			goto bail;
		arg = arg->next;
		param = param->next;
	}
	host->flags |= function->flags;
	host->params = (txNode*)args;
	fxPopNode(parser);
	fxPushNode(parser, (txNode*)host);
	return;
bail:
    fxParserReportWarning(parser, function->line, "cannot optimize native");
}

txBoolean fxCheckReference(txParser* parser, txToken theToken)
{
	txNode* node = parser->root;
	txToken aToken = (node && node->description) ? node->description->token : XS_NO_TOKEN;
	if (aToken == XS_TOKEN_EXPRESSIONS) {
		txNode* item;
	again:
		item = ((txExpressionsNode*)node)->items->first;
		if (item && !item->next) {
			aToken = (item->description) ? item->description->token : XS_NO_TOKEN;
			if ((aToken == XS_TOKEN_ACCESS) || (aToken == XS_TOKEN_MEMBER) || (aToken == XS_TOKEN_MEMBER_AT) || (aToken == XS_TOKEN_PRIVATE_MEMBER) || (aToken == XS_TOKEN_UNDEFINED)) {
				item->next = node->next;
				node = parser->root = item;
			}
			else if (aToken == XS_TOKEN_EXPRESSIONS) {
				item->next = node->next;
				node = item;
				goto again;
			}
			else
				aToken = XS_TOKEN_EXPRESSIONS;
		}
	}
	if (aToken == XS_TOKEN_ACCESS) {
		fxCheckStrictSymbol(parser, ((txAccessNode*)node)->symbol);
		return 1;
	}
	if ((aToken == XS_TOKEN_MEMBER) || (aToken == XS_TOKEN_MEMBER_AT) || (aToken == XS_TOKEN_PRIVATE_MEMBER) || (aToken == XS_TOKEN_UNDEFINED))
		return 1;
		
	if (theToken == XS_TOKEN_ASSIGN) {
		if (aToken == XS_TOKEN_ARRAY) {
			txNode* binding = fxArrayBindingFromExpression(parser, node, XS_TOKEN_ACCESS);
            if (binding) {
                binding->next = node->next;
                parser->root = binding;
 				return 1;
           }
       	}
		else if (aToken == XS_TOKEN_OBJECT) {
			txNode* binding = fxObjectBindingFromExpression(parser, node, XS_TOKEN_ACCESS);
            if (binding) {
                binding->next = node->next;
                parser->root = binding;
 				return 1;
            }
        }
	}
	else if (theToken == XS_TOKEN_DELETE)
		return 1;
	return 0;
}

void fxCheckStrictFunction(txParser* parser, txFunctionNode* function)
{
	parser->states[0].line = function->line;
	fxCheckStrictSymbol(parser, function->symbol);
	fxCheckStrictBinding(parser, function->params);
}

void fxCheckStrictSymbol(txParser* parser, txSymbol* symbol)
{
	if (parser->flags & mxStrictFlag) {
		if (symbol == parser->argumentsSymbol)
			fxReportParserError(parser, parser->states[0].line, "invalid arguments (strict mode)");
		else if (symbol == parser->evalSymbol)
			fxReportParserError(parser, parser->states[0].line, "invalid eval (strict mode)");
		else if (symbol == parser->yieldSymbol)
			fxReportParserError(parser, parser->states[0].line, "invalid yield (strict mode)");
	}
	else if (parser->flags & mxYieldFlag) {
		if (symbol == parser->yieldSymbol)
			fxReportParserError(parser, parser->states[0].line, "invalid yield");
	}
}

void fxCheckUniqueProperty(txParser* parser, txNode* base, txNode* current)
{
	return; // no more!
	if (current->description->token == XS_TOKEN_PROPERTY) {
		txPropertyNode* currentNode = (txPropertyNode*)current;
		while (base != current) {
			txPropertyNode* baseNode = (txPropertyNode*)base;
			if (baseNode->description->token == XS_TOKEN_PROPERTY) {
				if (baseNode->symbol == currentNode->symbol) {
					fxCheckUniquePropertyAux(parser, (txNode*)baseNode, (txNode*)currentNode);			
				}
			}
			base = base->next;
		}
	}
	else {
		txPropertyAtNode* currentNode = (txPropertyAtNode*)current;
		txIntegerNode* currentAt = (txIntegerNode*)(currentNode->at);
		if (currentAt->description->token == XS_TOKEN_INTEGER) {
			while (base != current) {
				txPropertyAtNode* baseNode = (txPropertyAtNode*)base;
				if (baseNode->description->token == XS_TOKEN_PROPERTY_AT) {
					txIntegerNode* baseAt = (txIntegerNode*)(baseNode->at);
					if (baseAt->description->token == XS_TOKEN_INTEGER) {
						if (baseAt->value == currentAt->value) {
							fxCheckUniquePropertyAux(parser, (txNode*)baseNode, (txNode*)currentNode);			
						}
					}
				}
				base = base->next;
			}
		}
	}
}

void fxCheckUniquePropertyAux(txParser* parser, txNode* baseNode, txNode* currentNode)
{
	if (currentNode->flags & mxGetterFlag) {
		if (baseNode->flags & mxGetterFlag)
			fxReportParserError(parser, parser->states[0].line, "getter already defined");
		else if (!(baseNode->flags & mxSetterFlag))
			fxReportParserError(parser, parser->states[0].line, "property already defined");
	}
	else if (currentNode->flags & mxSetterFlag) {
		if (baseNode->flags & mxSetterFlag)
			fxReportParserError(parser, parser->states[0].line, "setter already defined");
		else if (!(baseNode->flags & mxGetterFlag))
			fxReportParserError(parser, parser->states[0].line, "property already defined");
	}
	else {
		if (baseNode->flags & mxGetterFlag)
			fxReportParserError(parser, parser->states[0].line, "getter already defined");
		else if (baseNode->flags & mxSetterFlag)
			fxReportParserError(parser, parser->states[0].line, "setter already defined");
		else if (parser->flags & mxStrictFlag)
			fxReportParserError(parser, parser->states[0].line, "property already defined (strict mode)");
	}
}

void fxJSXAttributeName(txParser* parser)
{
	txSymbol* symbol = parser->states[0].symbol;
	fxGetNextToken(parser);
	while (parser->states[0].token == XS_TOKEN_SUBTRACT) {
		fxGetNextToken(parser);
		if (gxTokenFlags[parser->states[0].token] & XS_TOKEN_IDENTIFIER_NAME)
			symbol = fxJSXName(parser, symbol, parser->states[0].symbol);
		else
			fxReportParserError(parser, parser->states[0].line, "missing name");
		fxGetNextToken(parser);
	}
	if (parser->states[0].token == XS_TOKEN_COLON) {
		fxGetNextToken(parser);
		if (gxTokenFlags[parser->states[0].token] & XS_TOKEN_IDENTIFIER_NAME)
			symbol = fxJSXNamespace(parser, symbol, parser->states[0].symbol);
		else
			fxReportParserError(parser, parser->states[0].line, "missing name");
		fxGetNextToken(parser);
		while (parser->states[0].token == XS_TOKEN_SUBTRACT) {
			fxGetNextToken(parser);
			if (gxTokenFlags[parser->states[0].token] & XS_TOKEN_IDENTIFIER_NAME)
				symbol = fxJSXName(parser, symbol, parser->states[0].symbol);
			else
				fxReportParserError(parser, parser->states[0].line, "missing name");
			fxGetNextToken(parser);
		}	
	}
	fxPushSymbol(parser, symbol);
}

void fxJSXElementName(txParser* parser)
{
	txInteger line = parser->states[0].line;
	txSymbol* symbol = parser->states[0].symbol;
	fxGetNextToken(parser);
	if (parser->states[0].token == XS_TOKEN_COLON) {
		fxGetNextToken(parser);
		if (parser->states[0].token == XS_TOKEN_IDENTIFIER)
			symbol = fxJSXNamespace(parser, symbol, parser->states[0].symbol);
		else
			fxReportParserError(parser, parser->states[0].line, "missing name");
		fxPushSymbol(parser, symbol);
		fxPushNodeStruct(parser, 1, XS_TOKEN_ACCESS, line);
		fxGetNextToken(parser);
	}
	else {
		fxPushSymbol(parser, symbol);
		fxPushNodeStruct(parser, 1, XS_TOKEN_ACCESS, line);
		while (parser->states[0].token == XS_TOKEN_DOT) {
			fxGetNextToken(parser);
			if (parser->states[0].token == XS_TOKEN_IDENTIFIER) {
				fxPushSymbol(parser, parser->states[0].symbol);
				fxPushNodeStruct(parser, 2, XS_TOKEN_MEMBER, parser->states[0].line);
				fxGetNextToken(parser);
			}
			else
				fxReportParserError(parser, parser->states[0].line, "missing property");
		}
	}
}


txSymbol* fxJSXNamespace(txParser* parser, txSymbol* namespace, txSymbol* name)
{
	txSize namespaceLength = namespace->length;
	txSize nameLength = name->length;
	txSize length = namespaceLength + 1 + nameLength + 1;
	txString string = fxNewParserChunk(parser, length);
	snprintf(string, length, "%s:%s", namespace->string, name->string);
	return fxNewParserSymbol(parser, string);
}

txBoolean fxJSXMatch(txParser* parser, txNode* opening, txNode* closing)
{
	if (opening && closing) {
		while (opening->description->token == XS_TOKEN_MEMBER) {
			if (closing->description->token != XS_TOKEN_MEMBER)
				return 0;
			if (((txMemberNode*)opening)->symbol != ((txMemberNode*)closing)->symbol)
				return 0;
			opening = ((txMemberNode*)opening)->reference;
			closing = ((txMemberNode*)closing)->reference;
		}
		if (opening->description->token == XS_TOKEN_ACCESS) {
			if (closing->description->token != XS_TOKEN_ACCESS)
				return 0;
			if (((txAccessNode*)opening)->symbol != ((txAccessNode*)closing)->symbol)
				return 0;
			return 1;
		}
	}
	return 0;
}

void fxJSONModule(txParser* parser)
{
	fxJSONValue(parser);
	
	fxPushSymbol(parser, parser->defaultSymbol);
	fxPushNULL(parser);
	fxPushNodeStruct(parser, 2, XS_TOKEN_CONST, 1);
	fxSwapNodes(parser);
	fxPushNodeStruct(parser, 2, XS_TOKEN_ASSIGN, 1);
	fxPushNodeStruct(parser, 1, XS_TOKEN_STATEMENT, 1);
	
	fxPushSymbol(parser, parser->defaultSymbol);
	fxPushNULL(parser);
	fxPushNodeStruct(parser, 2, XS_TOKEN_SPECIFIER, 1);
	fxPushNodeList(parser, 1);
	fxPushNULL(parser);
	fxPushNULL(parser);
	fxPushNodeStruct(parser, 3, XS_TOKEN_EXPORT, 1);
	
	fxPushNodeList(parser, 2);
	fxPushNodeStruct(parser, 1, XS_TOKEN_STATEMENTS, 1);
	
	fxPushNodeStruct(parser, 1, XS_TOKEN_MODULE, 1);
}

/* xs_no_recursion R22: the program parse is a K_PROGRAM TRAMPOLINE
   FRAME (fxProgramStep below) instead of a C loop. The old fxProgram
   ran its statement loop in native C, which cannot survive a pump
   pause (R22 stepwise parsing): after a budgeted pause the loop's
   locals would be gone while parser frames stayed pending. As a frame,
   the loop state lives in the parser — the whole program parse is
   resumable at any pump step. Statement semantics are UNCHANGED:
   same use-strict scan, same stuck-recovery, same node pushes. */
static void fxProgramStep(txParser* parser);

void fxProgram(txParser* parser)
{
	fxParserCall(parser, K_PROGRAM, parser->states[0].line);
}

/* K_PROGRAM frame body. Frame slots: i1 = nodeCount at entry (count0),
   i0 = states[0].line at entry, i2/i3 = stuck-check temps, u0 = phase
   (0 = use-strict scan, 1 = recovery loop). pc map:
     0 init · 1 phase-0 want-statement · 2 phase-0 statement completed
     (use-strict check) · 3 phase-1 want-statement · 5 phase-1 statement
     completed (stuck check) · 4 finalize. */
static void fxProgramStep(txParser* parser)
{
	txParserFrame* f = P_FRAME;
	txNode* node;
	switch (f->pc) {
	case 0:
		f->i1 = parser->nodeCount;
		f->i0 = (txInteger)parser->states[0].line;
		f->u0 = 0;
		f->pc = 1;
		return;
	case 1:
		if (parser->states[0].token == XS_TOKEN_EOF) {
			f->pc = 4;
			return;
		}
		f->i2 = parser->nodeCount;
		f->i3 = (txInteger)parser->states[0].line;
		fxStatement(parser, -1);
		P_RESUME(2);
	case 2:
		/* a phase-0 statement completed: "use strict" detection. Stock
		   semantics: the scan CONTINUES (next fxStatement) unless this was
		   not a string statement — i.e. only a NON-string statement ends
		   phase 0. A "use strict"-like string directive that merely is not
		   "use strict" (or an escaped one) STAYS in phase 0. */
		node = parser->root;
		if (!node || !node->description || (node->description->token != XS_TOKEN_STATEMENT))
			P_RESUME(3);
		node = ((txStatementNode*)node)->expression;
		if (!node || !node->description || (node->description->token != XS_TOKEN_STRING))
			P_RESUME(3);
		if (!(node->flags & mxStringEscapeFlag) && (c_strcmp(((txStringNode*)node)->value, "use strict") == 0)) {
			if (!(parser->flags & mxStrictFlag)) {
				parser->flags |= mxStrictFlag;
				if (parser->states[0].token == XS_TOKEN_IDENTIFIER)
					fxCheckStrictKeyword(parser);
			}
		}
		f->pc = 1;
		return;
	case 3:
		if (parser->states[0].token == XS_TOKEN_EOF) {
			f->pc = 4;
			return;
		}
		f->i2 = parser->nodeCount;
		f->i3 = (txInteger)parser->states[0].line;
		fxStatement(parser, -1);
		P_RESUME(5);
	case 5:
		if (((txInteger)parser->nodeCount == f->i2) &&
			((txInteger)parser->states[0].line == f->i3)) {
			fxReportParserError(parser, parser->states[0].line, "parser stuck");
			f->pc = 4;
			return;
		}
		f->pc = 3;
		return;
	case 4:
		{
			txInteger count = parser->nodeCount - f->i1;
			txInteger line = f->i0;
			if (count > 1) {
				fxPushNodeList(parser, count);
				fxPushNodeStruct(parser, 1, XS_TOKEN_STATEMENTS, line);
			}
			else if (count == 0) {
				fxPushNodeStruct(parser, 0, XS_TOKEN_UNDEFINED, line);
				fxPushNodeStruct(parser, 1, XS_TOKEN_STATEMENT, line);
			}
			fxPushNodeStruct(parser, 1, XS_TOKEN_PROGRAM, line);
			if (parser->flags & mxFieldFlag)
				if (parser->flags & mxArgumentsFlag)
					fxReportParserError(parser, parser->states[0].line, "invalid arguments");
			parser->root->flags = parser->flags & mxStrictFlag;
		}
		break;
	}
	fxParserReturn(parser);
}

void fxModule(txParser* parser)
{
	txInteger aCount = parser->nodeCount;
	txInteger aLine = parser->states[0].line;
	while ((parser->states[0].token != XS_TOKEN_EOF)) {
		if (parser->states[0].token == XS_TOKEN_EXPORT)
			fxExportDeclaration(parser);
		else if (parser->states[0].token == XS_TOKEN_IMPORT) {
			fxLookAheadOnce(parser);
			if ((parser->states[1].token == XS_TOKEN_DOT) || (parser->states[1].token == XS_TOKEN_LEFT_PARENTHESIS))
				fxStatement(parser, 1);
			else
				fxImportDeclaration(parser);
		}
		else if (parser->states[0].token == XS_TOKEN_RETURN) {
			fxReportParserError(parser, parser->states[0].line, "invalid return");
			fxGetNextToken(parser);
		}
		else if (parser->states[0].token == XS_TOKEN_YIELD) {
			fxReportParserError(parser, parser->states[0].line, "invalid yield");
			fxGetNextToken(parser);
		}
		else {
			fxStatement(parser, 1);
		}
	}
	aCount = parser->nodeCount - aCount;
	if (aCount > 1) {
		fxPushNodeList(parser, aCount);
		fxPushNodeStruct(parser, 1, XS_TOKEN_STATEMENTS, aLine);
	}
	else if (aCount == 0) {
		fxPushNodeStruct(parser, 0, XS_TOKEN_UNDEFINED, aLine);
		fxPushNodeStruct(parser, 1, XS_TOKEN_STATEMENT, aLine);
	}
	fxPushNodeStruct(parser, 1, XS_TOKEN_MODULE, aLine);
	parser->root->flags = parser->flags & (mxStrictFlag | mxAwaitingFlag);
}

/* xs_no_recursion: fxJSONValue/fxJSONObject/fxJSONArray route through the
   parser trampoline (step functions near the top of this file). The native
   stock bodies appended here were removed: they recurse natively and the
   fxJSONObject copy was truncated. */

void fxJSONValue(txParser* parser)
{
	fxParserCall(parser, K_JSON_VALUE, parser->states[0].line);
}

void fxJSONObject(txParser* parser)
{
	fxParserCall(parser, K_JSON_OBJECT, parser->states[0].line);
}

void fxJSONArray(txParser* parser)
{
	fxParserCall(parser, K_JSON_ARRAY, parser->states[0].line);
}


/* xs_no_recursion: export/import grammar helpers restored verbatim from
   stock (their sub-calls route through trampoline forwarders, so these
   bodies only use constant C stack; they run inside trampoline frames as
   the fxProgram/fxModule frame bodies or at top level from xsTree.c). */

static void fxExportDeclaration(txParser* parser)
{
	txSymbol* symbol = C_NULL;
	txInteger count;
	txInteger line = parser->states[0].line;
	txUnsigned flag = 0;
	txToken aToken;
	fxMatchToken(parser, XS_TOKEN_EXPORT);
	switch (parser->states[0].token) {
	case XS_TOKEN_MULTIPLY:
		fxPushNULL(parser);
		fxGetNextToken(parser);
		if (fxIsKeyword(parser, parser->asSymbol)) {
			fxGetNextToken(parser);
			if (gxTokenFlags[parser->states[0].token] & XS_TOKEN_IDENTIFIER_NAME) {
				fxPushSymbol(parser, parser->states[0].symbol);
				fxGetNextToken(parser);
			}
			else {
				fxPushNULL(parser);
				fxReportParserError(parser, parser->states[0].line, "missing identifier");
			}
		}
		else {
			fxPushNULL(parser);
		}
		if (fxIsKeyword(parser, parser->fromSymbol)) {
			fxGetNextToken(parser);
			if (parser->states[0].token == XS_TOKEN_STRING) {
				fxPushNodeStruct(parser, 2, XS_TOKEN_SPECIFIER, line);
				fxPushNodeList(parser, 1);
				fxPushStringNode(parser, parser->states[0].stringLength, parser->states[0].string, line);
				fxGetNextToken(parser);
				if (!parser->states[0].crlf && (parser->states[0].token == XS_TOKEN_WITH)) {
					fxGetNextToken(parser);
					fxWithAttributes(parser);
				}
				else
					fxPushNULL(parser);
				fxPushNodeStruct(parser, 3, XS_TOKEN_EXPORT, line);
				fxSemicolon(parser);
			}
			else
				fxReportParserError(parser, parser->states[0].line, "missing module");
		}
		else
			fxReportParserError(parser, parser->states[0].line, "missing from");
		break;
	case XS_TOKEN_DEFAULT:
		fxMatchToken(parser, XS_TOKEN_DEFAULT);
		if (parser->flags & mxDefaultFlag)
			fxReportParserError(parser, parser->states[0].line, "invalid default");
		parser->flags |= mxDefaultFlag;
		if (parser->states[0].token == XS_TOKEN_CLASS) {
			fxClassExpression(parser, line, &symbol);
			if (symbol)
				fxPushSymbol(parser, symbol);
			else
				fxPushSymbol(parser, parser->defaultSymbol);
			fxPushNodeStruct(parser, 1, XS_TOKEN_LET, line);
			fxSwapNodes(parser);
			fxPushNodeStruct(parser, 2, XS_TOKEN_BINDING, line);
		}
		else if (parser->states[0].token == XS_TOKEN_FUNCTION) {
	again:
			fxMatchToken(parser, XS_TOKEN_FUNCTION);
			if (parser->states[0].token == XS_TOKEN_MULTIPLY) {
				fxGetNextToken(parser);
				fxGeneratorExpression(parser, line, &symbol, flag);
			}
			else
				fxFunctionExpression(parser, line, &symbol, flag);
			if (symbol) {
				txDefineNode* node = fxDefineNodeNew(parser, XS_TOKEN_DEFINE, symbol);
				node->initializer = fxPopNode(parser);
				fxPushNode(parser, (txNode*)node);
			}
			else {
				txDefineNode* node = fxDefineNodeNew(parser, XS_TOKEN_DEFINE, parser->defaultSymbol);
				node->initializer = fxPopNode(parser);
				fxPushNode(parser, (txNode*)node);
			}
		}
		else {
			if ((parser->states[0].token == XS_TOKEN_IDENTIFIER) && (parser->states[0].symbol == parser->asyncSymbol) && (!parser->states[0].escaped)) {
				fxLookAheadOnce(parser);
				if ((!parser->states[1].crlf) && (parser->states[1].token == XS_TOKEN_FUNCTION)) {
					fxGetNextToken(parser);
					flag = mxAsyncFlag;
					goto again;
				}
			}
			fxAssignmentExpression(parser);
			fxSemicolon(parser);
			fxPushSymbol(parser, parser->defaultSymbol);
			fxPushNULL(parser);
			fxPushNodeStruct(parser, 2, XS_TOKEN_CONST, line);
			fxSwapNodes(parser);
			fxPushNodeStruct(parser, 2, XS_TOKEN_ASSIGN, line);
			fxPushNodeStruct(parser, 1, XS_TOKEN_STATEMENT, line);
		}
		if (symbol) {
			fxPushSymbol(parser, symbol);
			fxPushSymbol(parser, parser->defaultSymbol);
		}
		else {
			fxPushSymbol(parser, parser->defaultSymbol);
			fxPushNULL(parser);
		}
		fxPushNodeStruct(parser, 2, XS_TOKEN_SPECIFIER, line);
		fxPushNodeList(parser, 1);
		fxPushNULL(parser);
		fxPushNULL(parser);
		fxPushNodeStruct(parser, 3, XS_TOKEN_EXPORT, line);
		break;
	case XS_TOKEN_CLASS:
		fxClassExpression(parser, line, &symbol);
		if (symbol) {
			fxPushSymbol(parser, symbol);
			fxPushNodeStruct(parser, 1, XS_TOKEN_LET, line);
			fxSwapNodes(parser);
			fxPushNodeStruct(parser, 2, XS_TOKEN_BINDING, line);

			fxPushSymbol(parser, symbol);
			fxPushNULL(parser);
			fxPushNodeStruct(parser, 2, XS_TOKEN_SPECIFIER, line);
			fxPushNodeList(parser, 1);
			fxPushNULL(parser);
			fxPushNULL(parser);
			fxPushNodeStruct(parser, 3, XS_TOKEN_EXPORT, line);
		}
		else
			fxReportParserError(parser, parser->states[0].line, "missing identifier");
		break;
	case XS_TOKEN_FUNCTION:
	again2:
		fxMatchToken(parser, XS_TOKEN_FUNCTION);
		if (parser->states[0].token == XS_TOKEN_MULTIPLY) {
			fxGetNextToken(parser);
			fxGeneratorExpression(parser, line, &symbol, flag);
		}
		else
			fxFunctionExpression(parser, line, &symbol, flag);
		if (symbol) {
			txDefineNode* node = fxDefineNodeNew(parser, XS_TOKEN_DEFINE, symbol);
			node->initializer = fxPopNode(parser);
			fxPushNode(parser, (txNode*)node);
			fxPushSymbol(parser, symbol);
			fxPushNULL(parser);
			fxPushNodeStruct(parser, 2, XS_TOKEN_SPECIFIER, line);
			fxPushNodeList(parser, 1);
			fxPushNULL(parser);
			fxPushNULL(parser);
			fxPushNodeStruct(parser, 3, XS_TOKEN_EXPORT, line);
		}
		else
			fxReportParserError(parser, parser->states[0].line, "missing identifier");
		break;
	case XS_TOKEN_CONST:
	case XS_TOKEN_LET:
	case XS_TOKEN_VAR:
		aToken = parser->states[0].token;
		fxVariableStatement(parser, aToken, 0);
		count = parser->nodeCount;
		fxExportBinding(parser, parser->root);
		fxPushNodeList(parser, parser->nodeCount - count);
		fxPushNULL(parser);
		fxPushNULL(parser);
		fxPushNodeStruct(parser, 3, XS_TOKEN_EXPORT, line);
		fxSemicolon(parser);
		break;
	case XS_TOKEN_LEFT_BRACE:
		count = parser->nodeCount;
		fxSpecifiers(parser);
		fxPushNodeList(parser, parser->nodeCount - count);
		if (fxIsKeyword(parser, parser->fromSymbol)) {
			fxGetNextToken(parser);
			if (parser->states[0].token == XS_TOKEN_STRING) {
				fxPushStringNode(parser, parser->states[0].stringLength, parser->states[0].string, line);
				fxGetNextToken(parser);
			}
			else {
				fxPushNULL(parser);
				fxReportParserError(parser, parser->states[0].line, "missing module");
			}
			if (!parser->states[0].crlf && (parser->states[0].token == XS_TOKEN_WITH)) {
				fxGetNextToken(parser);
				fxWithAttributes(parser);
			}
			else
				fxPushNULL(parser);
		}
		else {
			fxPushNULL(parser);
			fxPushNULL(parser);
		}
		fxPushNodeStruct(parser, 3, XS_TOKEN_EXPORT, line);
		fxSemicolon(parser);
		break;
	default:
		if ((parser->states[0].token == XS_TOKEN_IDENTIFIER) && (parser->states[0].symbol == parser->asyncSymbol) && (!parser->states[0].escaped)) {
			fxLookAheadOnce(parser);
			if ((!parser->states[1].crlf) && (parser->states[1].token == XS_TOKEN_FUNCTION)) {
				fxGetNextToken(parser);
				flag = mxAsyncFlag;
				goto again2;
			}
		}
		fxReportParserError(parser, parser->states[0].line, "invalid export %s", gxTokenNames[parser->states[0].token]);
		fxGetNextToken(parser);
		break;
	}
}

static void fxExportBinding(txParser* parser, txNode* node)
{
	txToken token = node->description->token;
	if ((token == XS_TOKEN_CONST) || (token == XS_TOKEN_LET) || (token == XS_TOKEN_VAR)) {
		fxPushSymbol(parser, ((txDeclareNode*)node)->symbol);
		fxPushNULL(parser);
		fxPushNodeStruct(parser, 2, XS_TOKEN_SPECIFIER, node->line);
	}
	else if (token == XS_TOKEN_BINDING) {
		fxExportBinding(parser, ((txBindingNode*)node)->target);
	}
	else if (token == XS_TOKEN_ARRAY_BINDING) {
		node = ((txArrayBindingNode*)node)->items->first;
		while (node) {
			fxExportBinding(parser, node);
			node = node->next;
		}
	}
	else if (token == XS_TOKEN_OBJECT_BINDING) {
		node = ((txObjectBindingNode*)node)->items->first;
		while (node) {
			fxExportBinding(parser, node);
			node = node->next;
		}
	}
	else if (token == XS_TOKEN_PROPERTY_BINDING)
		fxExportBinding(parser, ((txPropertyBindingNode*)node)->binding);
	else if (token == XS_TOKEN_PROPERTY_BINDING_AT)
		fxExportBinding(parser, ((txPropertyBindingAtNode*)node)->binding);
	else if (token == XS_TOKEN_REST_BINDING)
		fxExportBinding(parser, ((txRestBindingNode*)node)->binding);
	else if (token == XS_TOKEN_STATEMENTS) {
		node = ((txStatementsNode*)node)->items->first;
		while (node) {
			fxExportBinding(parser, node);
			node = node->next;
		}
	}
}

static void fxImportDeclaration(txParser* parser)
{
	txBoolean asFlag = 1;
	txBoolean fromFlag = 0;
	txInteger count = parser->nodeCount;
	fxMatchToken(parser, XS_TOKEN_IMPORT);
	if (parser->states[0].token == XS_TOKEN_IDENTIFIER) {
		fxPushSymbol(parser, parser->defaultSymbol);
		fxPushSymbol(parser, parser->states[0].symbol);
		fxPushNodeStruct(parser, 2, XS_TOKEN_SPECIFIER, parser->states[0].line);
		fxGetNextToken(parser);
		if (parser->states[0].token == XS_TOKEN_COMMA)
			fxGetNextToken(parser);
		else
			asFlag = 0;
		fromFlag = 1;
	}
	if (asFlag) {
		if (parser->states[0].token == XS_TOKEN_MULTIPLY) {
			fxGetNextToken(parser);
			if (fxIsKeyword(parser, parser->asSymbol)) {
				fxGetNextToken(parser);
				if (parser->states[0].token == XS_TOKEN_IDENTIFIER) {
					fxPushNULL(parser);
					fxPushSymbol(parser, parser->states[0].symbol);
					fxPushNodeStruct(parser, 2, XS_TOKEN_SPECIFIER, parser->states[0].line);
					fxGetNextToken(parser);
				}
				else {
					fxReportParserError(parser, parser->states[0].line, "missing identifier");
				}
			}
			else {
				fxReportParserError(parser, parser->states[0].line, "missing as");
			}
			fromFlag = 1;
		}
		else if (parser->states[0].token == XS_TOKEN_LEFT_BRACE) {
			fxSpecifiers(parser);
			fromFlag = 1;
		}
	}
	fxPushNodeList(parser, parser->nodeCount - count);
	if (fromFlag) {
		if (fxIsKeyword(parser, parser->fromSymbol)) {
			fxGetNextToken(parser);
			if (parser->states[0].token == XS_TOKEN_STRING) {
				fxPushStringNode(parser, parser->states[0].stringLength, parser->states[0].string, parser->states[0].line);
				fxGetNextToken(parser);
			}
			else {
				fxPushNULL(parser);
				fxReportParserError(parser, parser->states[0].line, "missing module");
			}
		}
		else {
			fxPushNULL(parser);
			fxReportParserError(parser, parser->states[0].line, "missing from");
		}
	}
	else if (parser->states[0].token == XS_TOKEN_STRING) {
		fxPushStringNode(parser, parser->states[0].stringLength, parser->states[0].string, parser->states[0].line);
		fxGetNextToken(parser);
	}
	else {
		fxPushNULL(parser);
		fxReportParserError(parser, parser->states[0].line, "missing module");
	}
	if (!parser->states[0].crlf && (parser->states[0].token == XS_TOKEN_WITH)) {
		fxGetNextToken(parser);
		fxWithAttributes(parser);
	}
	else
		fxPushNULL(parser);
	fxPushNodeStruct(parser, 3, XS_TOKEN_IMPORT, parser->states[0].line);
	fxSemicolon(parser);
}

static void fxSpecifiers(txParser* parser)
{
//	txInteger aCount = 0;
	fxMatchToken(parser, XS_TOKEN_LEFT_BRACE);
	while (gxTokenFlags[parser->states[0].token] & XS_TOKEN_IDENTIFIER_NAME) {
		fxPushSymbol(parser, parser->states[0].symbol);
		fxGetNextToken(parser);
		if (fxIsKeyword(parser, parser->asSymbol)) {
			fxGetNextToken(parser);
			if (gxTokenFlags[parser->states[0].token] & XS_TOKEN_IDENTIFIER_NAME) {
				fxPushSymbol(parser, parser->states[0].symbol);
				fxGetNextToken(parser);
			}
			else {
				fxPushNULL(parser);
				fxReportParserError(parser, parser->states[0].line, "missing identifier");
			}
		}
		else
			fxPushNULL(parser);
		fxPushNodeStruct(parser, 2, XS_TOKEN_SPECIFIER, parser->states[0].line);
//		aCount++;
		if (parser->states[0].token != XS_TOKEN_COMMA)
			break;
		fxGetNextToken(parser);
	}
	fxMatchToken(parser, XS_TOKEN_RIGHT_BRACE);
}

static void fxWithAttributes(txParser* parser)
{
	txBoolean flag = 0;
	txString string;
	fxMatchToken(parser, XS_TOKEN_LEFT_BRACE);
	while (parser->states[0].token != XS_TOKEN_RIGHT_BRACE) {
		if ((gxTokenFlags[parser->states[0].token] & XS_TOKEN_IDENTIFIER_NAME))
			string = parser->states[0].symbol->string;
		else if (parser->states[0].token == XS_TOKEN_STRING)
			string = parser->states[0].string;
		else {
			fxReportParserError(parser, parser->states[0].line, "missing attribute key");
			break;
		}
		if (c_strcmp(string, "type"))
			fxReportParserError(parser, parser->states[0].line, "invalid attribute key");
		else if (flag)
			fxReportParserError(parser, parser->states[0].line, "duplicate attribute");
		fxGetNextToken(parser);
		if (parser->states[0].token != XS_TOKEN_COLON) {
			fxReportParserError(parser, parser->states[0].line, "missing :");
			break;
		}
        fxGetNextToken(parser);
		if (parser->states[0].token == XS_TOKEN_STRING)
			string = parser->states[0].string;
		else {
			fxReportParserError(parser, parser->states[0].line, "missing attribute value");
			break;
		}
		if (c_strcmp(string, "json"))
			fxReportParserError(parser, parser->states[0].line, "invalid attribute value");
		flag = 1;
		fxGetNextToken(parser);
		if (parser->states[0].token != XS_TOKEN_COMMA)
			break;
		fxGetNextToken(parser);
	}
	fxMatchToken(parser, XS_TOKEN_RIGHT_BRACE);
	if (flag)
		fxPushSymbol(parser, parser->jsonSymbol);
	else
		fxPushNULL(parser);
}

/* xs_no_recursion: stock fxReportWarning renamed (machine-level symbol is
   macro-renamed to fxReportWarning_nr by the rename header). */
static void fxParserReportWarning(txParser* parser, txInteger line, txString theFormat, ...)
{
	c_va_list arguments;
	c_va_start(arguments, theFormat);
    (*parser->reportWarning)(parser->console, parser->path ? parser->path->string : C_NULL, line, theFormat, arguments);
	c_va_end(arguments);
}
