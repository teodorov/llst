/*
*    main.cpp
*
*    Program entry point
*
*    LLST (LLVM Smalltalk or Lo Level Smalltalk) version 0.1
*
*    LLST is
*        Copyright (C) 2012 by Dmitry Kashitsyn   aka Korvin aka Halt <korvin@deeptown.org>
*        Copyright (C) 2012 by Roman Proskuryakov aka Humbug          <humbug@deeptown.org>
*
*    LLST is based on the LittleSmalltalk which is
*        Copyright (C) 1987-2005 by Timothy A. Budd
*        Copyright (C) 2007 by Charles R. Childers
*        Copyright (C) 2005-2007 by Danny Reinhold
*
*    Original license of LittleSmalltalk may be found in the LICENSE file.
*
*
*    This file is part of LLST.
*    LLST is free software: you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    LLST is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with LLST.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <jit.h>

#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/IRReader.h>
#include <llvm/Analysis/Verifier.h>
#include <llvm/ExecutionEngine/GenericValue.h>

#include <llvm/PassManager.h>
#include <llvm/Target/TargetData.h>
#include <llvm/LinkAllPasses.h>

#include <iostream>
#include <sstream>

using namespace llvm;

JITRuntime* JITRuntime::s_instance = 0;

void JITRuntime::initialize(SmalltalkVM* softVM)
{
    s_instance = this;
    m_softVM = softVM;

    // Initializing LLVM subsystem
    InitializeNativeTarget();

    LLVMContext& llvmContext = getGlobalContext();

    // Initializing types module
    SMDiagnostic Err;
    m_TypeModule = ParseIRFile("../include/llvm_types.ll", Err, llvmContext); // FIXME Hardcoded path
    if (!m_TypeModule) {
        Err.print("JITRuntime.cpp", errs());
        exit(1);
    }

    // Initializing JIT module.
    // All JIT functions will be created here
    // m_JITModule = new Module("jit", llvmContext);
    m_JITModule = m_TypeModule;

    // Providing the memory management interface to the JIT module
    // FIXME Think about interfacing the MemoryManager directly
    // These are then used as an allocator function return types

    TargetOptions Opts;
    Opts.JITExceptionHandling = true;

    std::string error;
    m_executionEngine = EngineBuilder(m_JITModule)
                            .setEngineKind(EngineKind::JIT)
                            .setErrorStr(&error)
                            .setTargetOptions(Opts)
                            .create();
                            
    if (!m_executionEngine) {
        errs() << error;
        exit(1);
    }

    ot.initializeFromModule(m_TypeModule);

    initializeGlobals();
    initializePassManager();
    initializeRuntimeAPI();
    initializeExceptionAPI();

    // Initializing the method compiler
    m_methodCompiler = new MethodCompiler(m_JITModule, m_TypeModule, m_runtimeAPI, m_exceptionAPI);

    // Initializing stack
    memset(&m_blockFunctionLookupCache, 0, sizeof(m_blockFunctionLookupCache));
    memset(&m_functionLookupCache, 0, sizeof(m_functionLookupCache));
}

void JITRuntime::dumpJIT()
{
    verifyModule(*m_JITModule);
    m_JITModule->dump();
}

JITRuntime::~JITRuntime() {
    // Finalize stuff and dispose memory
    if (m_functionPassManager)
        delete m_functionPassManager;
}

TBlock* JITRuntime::createBlock(TContext* callingContext, uint8_t argLocation, uint16_t bytePointer)
{
    // Protecting pointer
    hptr<TContext> previousContext = m_softVM->newPointer(callingContext);

    // Creating new context object and inheriting context variables
    // NOTE We do not allocating stack because it's not used in LLVM
    hptr<TBlock> newBlock      = m_softVM->newObject<TBlock>();
    newBlock->argumentLocation = newInteger(argLocation);
    newBlock->bytePointer      = newInteger(bytePointer);
    newBlock->method           = previousContext->method;
    newBlock->arguments        = previousContext->arguments;
    newBlock->temporaries      = previousContext->temporaries;

    // Assigning creatingContext depending on the hierarchy
    // Nested blocks inherit the outer creating context
    if (previousContext->getClass() == globals.blockClass)
        newBlock->creatingContext = previousContext.cast<TBlock>()->creatingContext;
    else
        newBlock->creatingContext = previousContext;

    return newBlock;
}

JITRuntime::TMethodFunction JITRuntime::lookupFunctionInCache(TSymbol* selector, TClass* klass)
{
    uint32_t hash = reinterpret_cast<uint32_t>(selector) ^ reinterpret_cast<uint32_t>(klass);
    TFunctionCacheEntry& entry = m_functionLookupCache[hash % LOOKUP_CACHE_SIZE];
    
    if (entry.methodName    == selector && entry.receiverClass == klass) {
        m_cacheHits++;
        return entry.function;
    } else {
        m_cacheMisses++;
        return 0;
    }
}

JITRuntime::TBlockFunction JITRuntime::lookupBlockFunctionInCache(TSymbol* containerMethodName, TClass* containerMethodClass, uint32_t blockOffset)
{
    uint32_t hash = reinterpret_cast<uint32_t>(containerMethodName) ^ reinterpret_cast<uint32_t>(containerMethodClass) ^ blockOffset;
    TBlockFunctionCacheEntry& entry = m_blockFunctionLookupCache[hash % LOOKUP_CACHE_SIZE];
    
    if (entry.containerMethodName  == containerMethodName  && 
        entry.containerMethodClass == containerMethodClass && 
        entry.blockOffset == blockOffset) 
    {
        m_cacheHits++;
        return entry.function;
    } else {
        m_cacheMisses++;
        return 0;
    }
}

JITRuntime::TMethodFunction JITRuntime::updateFunctionCache(TSymbol* selector, TClass* klass, TMethodFunction function)
{
    uint32_t hash = reinterpret_cast<uint32_t>(selector) ^ reinterpret_cast<uint32_t>(klass);
    TFunctionCacheEntry& entry = m_functionLookupCache[hash % LOOKUP_CACHE_SIZE];
    
    entry.methodName = selector;
    entry.receiverClass = klass;
    entry.function = function;
}

JITRuntime::TMethodFunction JITRuntime::updateBlockFunctionCache(TSymbol* containerMethodName, TClass* containerMethodClass, uint32_t blockOffset, TBlockFunction function)
{
    uint32_t hash = reinterpret_cast<uint32_t>(containerMethodName) ^ reinterpret_cast<uint32_t>(containerMethodClass) ^ blockOffset;
    TBlockFunctionCacheEntry& entry = m_blockFunctionLookupCache[hash % LOOKUP_CACHE_SIZE];
    
    entry.containerMethodName  = containerMethodName;
    entry.containerMethodClass = containerMethodClass;
    entry.function = function;
}

TObject* JITRuntime::invokeBlock(TBlock* block, TContext* callingContext)
{
    // Guessing the block function name
    // TODO Fast 1-way lookup cache
    const uint16_t blockOffset = getIntegerValue(block->bytePointer);
    
    TBlockFunction compiledBlockFunction = lookupBlockFunctionInCache(block->method->name, block->method->klass, blockOffset);
    
    if (! compiledBlockFunction) {
        std::ostringstream ss;
        ss << block->method->klass->name->toString() << ">>" << block->method->name->toString() << "@" << blockOffset;
        std::string blockFunctionName = ss.str();
        
        llvm::Function* blockFunction = m_JITModule->getFunction(blockFunctionName);
        if (!blockFunction) {
            // Block functions are created when wrapping method gets compiled.
            // If function was not found then the whole method needs compilation.
            
            // Compiling function and storing it to the table for further use
            llvm::Function* methodFunction = m_methodCompiler->compileMethod(block->method, callingContext);
            blockFunction = m_JITModule->getFunction(blockFunctionName);
            if (!methodFunction || !blockFunction) {
                // Something is really wrong!
                outs() << "JIT: Fatal error in invokeBlock for " << blockFunctionName << "\n";
                exit(1);
            }
            
            verifyModule(*m_JITModule);
            // Running the optimization passes on a function
            //m_functionPassManager->run(*function);
        }
        
        // outs() << *blockFunction;
        compiledBlockFunction = reinterpret_cast<TBlockFunction>(m_executionEngine->getPointerToFunction(blockFunction));
        updateBlockFunctionCache(block->method->name, block->method->klass, blockOffset, compiledBlockFunction);
    }
    
    block->previousContext = callingContext->previousContext;
    TObject* result = compiledBlockFunction(block);
    
    //     printf("true = %p, false = %p, nil = %p\n", globals.trueObject, globals.falseObject, globals.nilObject);
    //     printf("Block function result: %p\n", result);
    //     printf("Result class: %s\n", isSmallInteger(result) ? "SmallInt" : result->getClass()->name->toString().c_str() );
    
    return result;
}

TObject* JITRuntime::sendMessage(TContext* callingContext, TSymbol* message, TObjectArray* arguments)
{
    // First of all we need to find the actual method object
    TObject* receiver = arguments->getField(0);
    TClass*  receiverClass = globals.smallIntClass;
    if (! isSmallInteger(receiver))
        receiverClass = receiver->getClass();

    // Searching for the actual method to be called
    hptr<TMethod> method = m_softVM->newPointer(m_softVM->lookupMethod(message, receiverClass));
    // TODO #doesNotUnderstand:

    if (! method.rawptr()) {
        outs() << "Method not found!";
        exit(1);
    }
    
    // Searching for the jit compiled function
    // TODO Fast 1-way lookup cache
    
    TMethodFunction compiledMethodFunction = lookupFunctionInCache(message, receiverClass); 
    
    if (! compiledMethodFunction) {
        // If function was not found in the cache looking it in the LLVM directly
        std::string functionName = method->klass->name->toString() + ">>" + method->name->toString();
        llvm::Function* methodFunction = m_JITModule->getFunction(functionName);
        
        if (! methodFunction) {
            // Compiling function and storing it to the table for further use
            methodFunction = m_methodCompiler->compileMethod(method, callingContext);

            //llvm::Function* asNumberBlock = m_JITModule->getFunction("String>>asNumber@4");
            //outs() << *asNumberBlock;
            //outs() << *methodFunction;

            verifyModule(*m_JITModule);
            // Running the optimization passes on a function
            //m_functionPassManager->run(*function);
        }

        //outs() << *m_JITModule;
        // Calling the method and returning the result
        //outs() << "Acquiring function address for " << functionName << " ...\n";
        compiledMethodFunction = reinterpret_cast<TMethodFunction>(m_executionEngine->getPointerToFunction(methodFunction));
        updateFunctionCache(message, receiverClass, compiledMethodFunction);
    }
    
    // Preparing the context objects. Because we do not call the software
    // implementation here, we do not need to allocate the stack object
    // because it is not used by JIT runtime. We also may skip the proper
    // initialization of various objects such as stackTop and bytePointer.

    // Protecting the pointers before allocation
    hptr<TObjectArray> messageArguments = m_softVM->newPointer(arguments);
    hptr<TContext>     previousContext  = m_softVM->newPointer(callingContext);

    // Creating context object and temporaries
    hptr<TContext>     newContext = m_softVM->newObject<TContext>();
    hptr<TObjectArray> newTemps   = m_softVM->newObject<TObjectArray>(getIntegerValue(method->temporarySize));

    // Initializing context variables
    newContext->temporaries       = newTemps;
    newContext->arguments         = messageArguments;
    newContext->method            = method;
    newContext->previousContext   = previousContext;

	//outs() << "Calling compiled method " << functionName << " ...\n";
	TObject* result = compiledMethodFunction(newContext);

//     printf("true = %p, false = %p, nil = %p\n", globals.trueObject, globals.falseObject, globals.nilObject);
//     printf("Function result: %p\n", result);
//     printf("Result class: %s\n", isSmallInteger(result) ? "SmallInt" : result->getClass()->name->toString().c_str() );

    return result;
}

void JITRuntime::initializeGlobals() {
    GlobalValue* m_jitGlobals = cast<GlobalValue>( m_JITModule->getOrInsertGlobal("globals", ot.globals) );
    m_executionEngine->addGlobalMapping(m_jitGlobals, reinterpret_cast<void*>(&globals));

    GlobalValue* gNil = cast<GlobalValue>( m_JITModule->getOrInsertGlobal("globals.nilObject", ot.object) );
    m_executionEngine->addGlobalMapping(gNil, reinterpret_cast<void*>(globals.nilObject));

    GlobalValue* gTrue = cast<GlobalValue>( m_JITModule->getOrInsertGlobal("globals.trueObject", ot.object) );
    m_executionEngine->addGlobalMapping(gTrue, reinterpret_cast<void*>(globals.trueObject));

    GlobalValue* gFalse = cast<GlobalValue>( m_JITModule->getOrInsertGlobal("globals.falseObject", ot.object) );
    m_executionEngine->addGlobalMapping(gFalse, reinterpret_cast<void*>(globals.falseObject));

    GlobalValue* gSmallIntClass = cast<GlobalValue>( m_JITModule->getOrInsertGlobal("globals.smallIntClass", ot.klass) );
    m_executionEngine->addGlobalMapping(gSmallIntClass, reinterpret_cast<void*>(globals.smallIntClass));

    GlobalValue* gArrayClass = cast<GlobalValue>( m_JITModule->getOrInsertGlobal("globals.arrayClass", ot.klass) );
    m_executionEngine->addGlobalMapping(gArrayClass, reinterpret_cast<void*>(globals.arrayClass));

    GlobalValue* gmessageL = cast<GlobalValue>( m_JITModule->getOrInsertGlobal("globals.<", ot.symbol) );
    m_executionEngine->addGlobalMapping(gmessageL, reinterpret_cast<void*>(globals.binaryMessages[0]));

    GlobalValue* gmessageLE = cast<GlobalValue>( m_JITModule->getOrInsertGlobal("globals.<=", ot.symbol) );
    m_executionEngine->addGlobalMapping(gmessageLE, reinterpret_cast<void*>(globals.binaryMessages[1]));

    GlobalValue* gmessagePlus = cast<GlobalValue>( m_JITModule->getOrInsertGlobal("globals.+", ot.symbol) );
    m_executionEngine->addGlobalMapping(gmessagePlus, reinterpret_cast<void*>(globals.binaryMessages[2]));
}

void JITRuntime::initializePassManager() {
    m_functionPassManager = new FunctionPassManager(m_JITModule);

    // Set up the optimizer pipeline.
    // Start with registering info about how the
    // target lays out data structures.
    /*m_functionPassManager->add(new TargetData(*m_executionEngine->getTargetData()));

    // Basic AliasAnslysis support for GVN.
    m_functionPassManager->add(llvm::createBasicAliasAnalysisPass());

    // Promote allocas to registers.
    m_functionPassManager->add(llvm::createPromoteMemoryToRegisterPass());

    // Do simple "peephole" optimizations and bit-twiddling optzns.
    m_functionPassManager->add(llvm::createInstructionCombiningPass());

    // Reassociate expressions.
    m_functionPassManager->add(llvm::createReassociatePass());

    // Eliminate Common SubExpressions.
    m_functionPassManager->add(llvm::createGVNPass());

    // Simplify the control flow graph (deleting unreachable
    // blocks, etc).
    m_functionPassManager->add(llvm::createCFGSimplificationPass()); */

//     m_functionPassManager->add(llvm::createDeadCodeEliminationPass());
//     m_functionPassManager->add(llvm::createDeadInstEliminationPass());
//     m_functionPassManager->add(llvm::createDeadStoreEliminationPass());

    m_functionPassManager->doInitialization();
}

void JITRuntime::initializeRuntimeAPI() {
    LLVMContext& llvmContext = getGlobalContext();

    PointerType* objectType     = ot.object->getPointerTo();
    PointerType* classType      = ot.klass->getPointerTo();
    PointerType* byteObjectType = ot.byteObject->getPointerTo();
    PointerType* contextType    = ot.context->getPointerTo();
    PointerType* blockType      = ot.block->getPointerTo();
    PointerType* objectSlotType = ot.object->getPointerTo()->getPointerTo(); // TObject**
    
    Type* params[] = {
        classType,                    // klass
        Type::getInt32Ty(llvmContext) // size
    };
    FunctionType* newOrdinaryObjectType = FunctionType::get(objectType,     params, false);
    FunctionType* newBinaryObjectType   = FunctionType::get(byteObjectType, params, false);


    Type* sendParams[] = {
        contextType,                    // callingContext
        ot.symbol->getPointerTo(),      // message selector
        ot.objectArray->getPointerTo()  // arguments
    };
    FunctionType* sendMessageType  = FunctionType::get(objectType, sendParams, false);

    Type* createBlockParams[] = {
        contextType,                  // callingContext
        Type::getInt8Ty(llvmContext), // argLocation
        Type::getInt16Ty(llvmContext) // bytePointer
    };
    FunctionType* createBlockType = FunctionType::get(blockType, createBlockParams, false);

    Type* invokeBlockParams[] = {
        blockType,  // block
        contextType // callingContext
    };
    FunctionType* invokeBlockType = FunctionType::get(objectType, invokeBlockParams, false);


    Type* emitBlockReturnParams[] = {
        objectType, // value
        contextType // targetContext
    };
    FunctionType* emitBlockReturnType = FunctionType::get(Type::getVoidTy(llvmContext), emitBlockReturnParams, false);

    Type* checkRootParams[] = {
        objectType,     // value
        objectSlotType  // slot
    };
    FunctionType* checkRootType = FunctionType::get(Type::getVoidTy(llvmContext), checkRootParams, false);
    
    Type* bulkReplaceParams[] = {
        objectType, // destination
        objectType, // sourceStartOffset
        objectType, // source
        objectType, // destinationStopOffset
        objectType  // destinationStartOffset
    };
    FunctionType* bulkReplaceType = FunctionType::get(Type::getInt1Ty(llvmContext), bulkReplaceParams, false);

    Type* performSmallIntParams[] = {
        Type::getInt8Ty(llvmContext), // opcode
        objectType,        // leftOperand
        objectType,        // rightOperand
    };
    FunctionType* performSmallIntType = FunctionType::get(objectType, performSmallIntParams, false);



    // Creating function references
    m_runtimeAPI.newOrdinaryObject  = Function::Create(newOrdinaryObjectType, Function::ExternalLinkage, "newOrdinaryObject", m_JITModule);
    m_runtimeAPI.newBinaryObject    = Function::Create(newBinaryObjectType, Function::ExternalLinkage, "newBinaryObject", m_JITModule);
    m_runtimeAPI.sendMessage        = Function::Create(sendMessageType, Function::ExternalLinkage, "sendMessage", m_JITModule);
    m_runtimeAPI.createBlock        = Function::Create(createBlockType, Function::ExternalLinkage, "createBlock", m_JITModule);
    m_runtimeAPI.invokeBlock        = Function::Create(invokeBlockType, Function::ExternalLinkage, "invokeBlock", m_JITModule);
    m_runtimeAPI.emitBlockReturn    = Function::Create(emitBlockReturnType, Function::ExternalLinkage, "emitBlockReturn", m_JITModule);
    m_runtimeAPI.checkRoot          = Function::Create(checkRootType, Function::ExternalLinkage, "checkRoot", m_JITModule );
    m_runtimeAPI.bulkReplace        = Function::Create(bulkReplaceType, Function::ExternalLinkage, "bulkReplace", m_JITModule);
    m_runtimeAPI.performSmallInt    = Function::Create(performSmallIntType, Function::ExternalLinkage, "performSmallInt", m_JITModule);

    // Mapping the function references to actual functions
    m_executionEngine->addGlobalMapping(m_runtimeAPI.newOrdinaryObject, reinterpret_cast<void*>(& ::newOrdinaryObject));
    m_executionEngine->addGlobalMapping(m_runtimeAPI.newBinaryObject, reinterpret_cast<void*>(& ::newBinaryObject));
    m_executionEngine->addGlobalMapping(m_runtimeAPI.sendMessage, reinterpret_cast<void*>(& ::sendMessage));
    m_executionEngine->addGlobalMapping(m_runtimeAPI.createBlock, reinterpret_cast<void*>(& ::createBlock));
    m_executionEngine->addGlobalMapping(m_runtimeAPI.invokeBlock, reinterpret_cast<void*>(& ::invokeBlock));
    m_executionEngine->addGlobalMapping(m_runtimeAPI.emitBlockReturn, reinterpret_cast<void*>(& ::emitBlockReturn));
    m_executionEngine->addGlobalMapping(m_runtimeAPI.checkRoot, reinterpret_cast<void*>(& ::checkRoot));
    m_executionEngine->addGlobalMapping(m_runtimeAPI.bulkReplace, reinterpret_cast<void*>(& ::bulkReplace));
    m_executionEngine->addGlobalMapping(m_runtimeAPI.performSmallInt, reinterpret_cast<void*>(& ::performSmallInt));
}

void JITRuntime::initializeExceptionAPI() {
    LLVMContext& llvmContext = getGlobalContext();

    m_exceptionAPI.gxx_personality = Function::Create(FunctionType::get(Type::getInt32Ty(llvmContext), true), Function::ExternalLinkage, "__gxx_personality_v0", m_JITModule);
    m_exceptionAPI.cxa_begin_catch = Function::Create(FunctionType::get(Type::getInt8PtrTy(llvmContext), Type::getInt8PtrTy(llvmContext), false), Function::ExternalLinkage, "__cxa_begin_catch", m_JITModule);
    m_exceptionAPI.cxa_end_catch   = Function::Create(FunctionType::get(Type::getVoidTy(llvmContext), false), Function::ExternalLinkage, "__cxa_end_catch", m_JITModule);
    m_exceptionAPI.cxa_rethrow     = Function::Create(FunctionType::get(Type::getVoidTy(llvmContext), false), Function::ExternalLinkage, "__cxa_rethrow", m_JITModule);
    
    m_exceptionAPI.blockReturnType = cast<GlobalValue>(m_JITModule->getOrInsertGlobal("blockReturnType", Type::getInt8Ty(llvmContext)));
    m_executionEngine->addGlobalMapping(m_exceptionAPI.blockReturnType, reinterpret_cast<void*>( TBlockReturn::getBlockReturnType() ));
}

extern "C" {

TObject* newOrdinaryObject(TClass* klass, uint32_t slotSize)
{
//     printf("newOrdinaryObject(%p '%s', %d)\n", klass, klass->name->toString().c_str(), slotSize);
    return JITRuntime::Instance()->getVM()->newOrdinaryObject(klass, slotSize);
}

TByteObject* newBinaryObject(TClass* klass, uint32_t dataSize)
{
//     printf("newBinaryObject(%p '%s', %d)\n", klass, klass->name->toString().c_str(), dataSize);
    return JITRuntime::Instance()->getVM()->newBinaryObject(klass, dataSize);
}

TObject* sendMessage(TContext* callingContext, TSymbol* message, TObjectArray* arguments)
{
//     printf("sendMessage(%p, #%s, %p)\n",
//            callingContext,
//            message->toString().c_str(),
//            arguments);

//     TObject* self = arguments->getField(0);
//     printf("\tself = %p\n", self);
//     
//     TClass* klass = isSmallInteger(self) ? globals.smallIntClass : self->getClass();
//     printf("\tself class = %p\n", klass);
//     printf("\tself class name = '%s'\n", klass->name->toString().c_str());
    
    return JITRuntime::Instance()->sendMessage(callingContext, message, arguments);
}

TBlock* createBlock(TContext* callingContext, uint8_t argLocation, uint16_t bytePointer)
{
//     printf("createBlock(%p, %d, %d)\n",
//         callingContext,
//         (uint32_t) argLocation,
//         (uint32_t) bytePointer );

    return JITRuntime::Instance()->createBlock(callingContext, argLocation, bytePointer);
}

TObject* invokeBlock(TBlock* block, TContext* callingContext)
{
//     printf("invokeBlock %p, %p\n", block, callingContext);
    return JITRuntime::Instance()->invokeBlock(block, callingContext);
}

void emitBlockReturn(TObject* value, TContext* targetContext)
{
//     printf("emitBlockReturn(%p, %p)\n", value, targetContext);
    throw TBlockReturn(value, targetContext);
}

void checkRoot(TObject* value, TObject** objectSlot)
{
//     printf("checkRoot %p, %p\n", value, objectSlot);
    JITRuntime::Instance()->getVM()->checkRoot(value, objectSlot);
}

bool bulkReplace(TObject* destination,
                TObject* destinationStartOffset,
                TObject* destinationStopOffset,
                TObject* source,
                TObject* sourceStartOffset)
{
    return JITRuntime::Instance()->getVM()->doBulkReplace(destination,
                                                        destinationStartOffset,
                                                        destinationStopOffset,
                                                        source,
                                                        sourceStartOffset);
}

TObject* performSmallInt(uint8_t opcode, TObject* leftObject, TObject* rightObject)
{
//     printf("performSmallInt %d, %p, %p\n", (uint32_t) opcode, leftObject, rightObject);

    int32_t leftOperand  = getIntegerValue(reinterpret_cast<TInteger>(leftObject));
    int32_t rightOperand = getIntegerValue(reinterpret_cast<TInteger>(rightObject));
    return JITRuntime::Instance()->getVM()->doSmallInt((SmalltalkVM::SmallIntOpcode) opcode, leftOperand, rightOperand);
}

}
