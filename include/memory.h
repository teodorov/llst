/*
 *    memory.h
 *
 *    LLST memory management routines and interfaces
 *
 *    LLST (LLVM Smalltalk or Low Level Smalltalk) version 0.4
 *
 *    LLST is
 *        Copyright (C) 2012-2015 by Dmitry Kashitsyn   <korvin@deeptown.org>
 *        Copyright (C) 2012-2015 by Roman Proskuryakov <humbug@deeptown.org>
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

#ifndef LLST_MEMORY_H_INCLUDED
#define LLST_MEMORY_H_INCLUDED

#include <cstddef>
#include <stdint.h>
#include <tr1/memory>
#include <types.h>
#include <vector>
#include <list>
#include <fstream>
#include "Timer.h"



struct TMemoryManagerHeapEvent {
    const std::string eventName;
    TDuration<TSec> timeDiff;
    uint32_t usedHeapSizeBeforeCollect;
    uint32_t usedHeapSizeAfterCollect;
    uint32_t totalHeapSize;
    TMemoryManagerHeapEvent() : eventName(), timeDiff(),
    usedHeapSizeBeforeCollect(0), usedHeapSizeAfterCollect(0), totalHeapSize(0) {}
    TMemoryManagerHeapEvent(std::string name) : eventName(name), timeDiff(),
    usedHeapSizeBeforeCollect(0), usedHeapSizeAfterCollect(0), totalHeapSize(0) {}
};

struct TMemoryManagerHeapInfo {
    uint32_t usedHeapSizeBeforeCollect;
    uint32_t usedHeapSizeAfterCollect;
    uint32_t totalHeapSize;
    std::list<TMemoryManagerHeapEvent> heapEvents;
    TMemoryManagerHeapInfo() :
        usedHeapSizeBeforeCollect(0),
        usedHeapSizeAfterCollect(0),
        totalHeapSize(0),
        heapEvents()
        {}
    bool empty() const {
        return usedHeapSizeBeforeCollect == 0 &&
                usedHeapSizeAfterCollect == 0 &&
                totalHeapSize == 0 &&
                heapEvents.empty();
    }
};

//represent three kinds of events in garbage collection log:
//just event, event which takes some time, event which interacting with a heap
struct TMemoryManagerEvent {
    const std::string eventName;
    TDuration<TSec> begin; //time spent from program start to event begin
    TDuration<TSec> timeDiff; //maybe null
    TMemoryManagerHeapInfo heapInfo; //maybe empty
    TMemoryManagerEvent(const std::string name): eventName(name), begin(), timeDiff(), heapInfo() {}
};

class IGCLogger {
public:
    virtual void writeLogLine(TMemoryManagerEvent event) = 0;
    virtual ~IGCLogger() {};
};

class GCLogger : public IGCLogger {
private:
    std::ofstream m_logFile;
public:
    GCLogger(const char* fileName);
    virtual ~GCLogger();
    virtual void writeLogLine(TMemoryManagerEvent event);
};

class EmptyGCLogger : public IGCLogger
{
public:
    EmptyGCLogger(){}
    virtual void writeLogLine(TMemoryManagerEvent){}
    virtual ~EmptyGCLogger() {}
};




// Memory manager statics is held
// in the following structure
struct TMemoryManagerInfo {
    uint32_t collectionsCount;
    uint32_t allocationsCount;
    uint64_t totalCollectionDelay;

    uint32_t leftToRightCollections;
    uint32_t rightToLeftCollections;
    uint64_t rightCollectionDelay;
    Timer timer;
    std::list<TMemoryManagerEvent> events;
    TMemoryManagerInfo():collectionsCount(0), allocationsCount(0), totalCollectionDelay(0),
    leftToRightCollections(0), rightToLeftCollections(0), rightCollectionDelay(0),
    timer(), events(){}
};

struct object_ptr {
    TObject* data;
    object_ptr* next;
    object_ptr() : data(0), next(0) {}
    explicit object_ptr(TObject* data)  : data(data), next(0) {}
    object_ptr& operator=(const object_ptr& value) { this->data = value.data; return *this; }
private:
    object_ptr(const object_ptr& value);
};

// Generic interface to a memory manager.
// Custom implementations such as BakerMemoryManager
// implement this interface.
class IMemoryManager {
protected:
    std::tr1::shared_ptr<IGCLogger> m_gcLogger;
    IMemoryManager(): m_gcLogger(new EmptyGCLogger()){}
public:
    virtual void setLogger(std::tr1::shared_ptr<IGCLogger> logger){
        m_gcLogger = logger;
    }

    virtual bool initializeHeap(std::size_t heapSize, std::size_t maxSize = 0) = 0;
    virtual bool initializeStaticHeap(std::size_t staticHeapSize) = 0;

    virtual void* allocate(std::size_t size, bool* collectionOccured = 0) = 0;
    virtual void* staticAllocate(std::size_t size) = 0;
    virtual void  collectGarbage() = 0;

    virtual bool  checkRoot(TObject* value, TObject** objectSlot) = 0;
    virtual void  addStaticRoot(TObject** pointer) = 0;
    virtual void  removeStaticRoot(TObject** pointer) = 0;
    virtual bool  isInStaticHeap(void* location) = 0;

    // External pointer handling
    virtual void  registerExternalHeapPointer(object_ptr& pointer) = 0;
    virtual void  releaseExternalHeapPointer(object_ptr& pointer) = 0;

    virtual uint32_t allocsBeyondCollection() = 0;
    virtual TMemoryManagerInfo getStat() = 0;

    virtual ~IMemoryManager() {};
};

// When pointer to a heap object is stored outside of the heap,
// specific actions need to be taken in order to prevent pointer
// invalidation due to GC procedure. External pointers need to be
// registered in GC so it will use this pointers as roots for the
// object traversing. GC will update the pointer data with the
// actual object location. hptr<> helps to organize external pointers
// by automatically calling registerExternalPointer() in constructor
// and releaseExternalPointer() in desctructor.
//
// External pointers are widely used in the VM execution code.
// VM provide helper functions newPointer() and newObject() which
// deal with hptr<> in a user friendly way. Use of these functions
// is highly recommended.

template <typename O> class hptr_base {
public:
    typedef O Object;

protected:
    object_ptr target;  // TODO static heap optimization && volatility
    IMemoryManager* mm; // TODO assign on copy operators
    bool isRegistered;  // TODO Pack flag into address
public:
    hptr_base(Object* object, IMemoryManager* mm, bool registerPointer = true)
    : target(object), mm(mm), isRegistered(registerPointer)
    {
        if (mm && registerPointer) mm->registerExternalHeapPointer(target);
            //mm->registerExternalPointer((TObject**) &target);
    }

    hptr_base(const hptr_base<Object>& pointer) : target(pointer.target.data), mm(pointer.mm), isRegistered(true)
    {
        if (mm) mm->registerExternalHeapPointer(target);
    }

    ~hptr_base() { if (mm && isRegistered) mm->releaseExternalHeapPointer(target); }

    //hptr_base<Object>& operator = (hptr_base<Object>& pointer) { target = pointer.target; return *this; }
    //hptr_base<Object>& operator = (Object* object) { target = object; return *this; }

    Object* rawptr() const { return static_cast<Object*>(target.data); }
    Object* operator -> () const { return static_cast<Object*>(target.data); }
    //Object& (operator*)() const { return *target; }
    operator Object*() const { return static_cast<Object*>(target.data); }

    template<typename C> C* cast() const { return static_cast<C*>(target.data); }
};

template <typename O> class hptr : public hptr_base<O> {
public:
    typedef O Object;
public:
    hptr(Object* object, IMemoryManager* mm, bool registerPointer = true) : hptr_base<Object>(object, mm, registerPointer) {}
    hptr(const hptr<Object>& pointer) : hptr_base<Object>(pointer) { }
    hptr<Object>& operator = (Object* object) { hptr_base<Object>::target.data = object; return *this; }

//     template<typename I>
//     Object& operator [] (I index) const { return hptr_base<Object>::target->operator[](index); }
};

// Hptr specialization for TArray<> class.
// Provides typed [] operator that allows
// convinient indexed access to the array contents
template <typename T> class hptr< TArray<T> > : public hptr_base< TArray<T> > {
public:
    typedef TArray<T> Object;
public:
    hptr(Object* object, IMemoryManager* mm, bool registerPointer = true) : hptr_base<Object>(object, mm, registerPointer) {}
    hptr(const hptr<Object>& pointer) : hptr_base<Object>(pointer) { }
    hptr<Object>& operator = (Object* object) { hptr_base<Object>::target.data = object; return *this; }

    template<typename I> T*& operator [] (I index) const { return hptr_base<Object>::target.data->operator[](index); }
};

// Hptr specialization for TByteObject.
// Provides typed [] operator that allows
// convinient indexed access to the bytearray contents
template <> class hptr<TByteObject> : public hptr_base<TByteObject> {
public:
    typedef TByteObject Object;
public:
    hptr(Object* object, IMemoryManager* mm, bool registerPointer = true) : hptr_base<Object>(object, mm, registerPointer) {}
    hptr(const hptr<Object>& pointer) : hptr_base<Object>(pointer) { }

    uint8_t& operator [] (uint32_t index) const { return static_cast<Object*>(target.data)->operator[](index); }
};

// Simple memory manager implementing classic baker two space algorithm.
// Each time two separate heaps are allocated but only one is active.
//
// When we need to allocate more memory but no space left on the current heap
// then garbage collection procedure takes place. It simply moves objects from
// the active heap to the inactive one and fixes the original pointers so they
// start directing to a new place. Collection is started from the root objects
// on the root stack and then on static allocated heap traversing reference tree in depth.
// When collection is done heaps are interchanged so the new one became active.
// All objects that were not moved during the collection are said to be disposed,
// so thier space may be reused by newly allocated ones.
//
class BakerMemoryManager : public IMemoryManager
{
protected:
    TMemoryManagerInfo m_memoryInfo;
    std::size_t m_heapSize;
    std::size_t m_maxHeapSize;

    uint8_t*  m_heapOne;
    uint8_t*  m_heapTwo;
    bool      m_activeHeapOne;

    uint8_t*  m_inactiveHeapBase;
    uint8_t*  m_inactiveHeapPointer;
    uint8_t*  m_activeHeapBase;
    uint8_t*  m_activeHeapPointer;

    std::size_t m_staticHeapSize;
    uint8_t*  m_staticHeapBase;
    uint8_t*  m_staticHeapPointer;


    struct TRootPointers {
        uint32_t size;
        uint32_t top;
        TObject* data[0];
    };

    // During GC we need to treat all objects in a very simple manner,
    // just as pointer holders. Class field is also a pointer so we
    // treat it just as one more object field.
    struct TMovableObject {
        TSize size;
        TMovableObject* data[0];

        TMovableObject(uint32_t dataSize, bool isBinary = false) : size(dataSize, isBinary) { }
    };

    /*virtual*/ TMovableObject* moveObject(TMovableObject* object);
    virtual void moveObjects();
    virtual void growHeap(uint32_t requestedSize);

    // These variables contain an array of pointers to objects from the
    // static heap to the dynamic one. Ihey are used during the GC
    // as a root for pointer iteration.

    // FIXME Temporary solution before GC will prove it's working
    //       Think about better memory organization
    typedef std::list<TMovableObject**> TStaticRoots;
    typedef std::list<TMovableObject**>::iterator TStaticRootsIterator;
    TStaticRoots m_staticRoots;

    // External pointers are typically managed by hptr<> template.
    // When pointer to a heap object is stored outside of the heap,
    // specific actions need to be taken in order to prevent pointer
    // invalidation. GC uses this information to correct external
    // pointers so they will point to correct location even after
    // garbage collection.
    object_ptr* m_externalPointersHead;
public:
    BakerMemoryManager();
    virtual ~BakerMemoryManager();

    virtual bool  initializeHeap(std::size_t heapSize, std::size_t maxHeapSize = 0);
    virtual bool  initializeStaticHeap(std::size_t staticHeapSize);
    virtual void* allocate(std::size_t requestedSize, bool* gcOccured = 0);
    virtual void* staticAllocate(std::size_t requestedSize);
    virtual void  collectGarbage();

    virtual bool  checkRoot(TObject* value, TObject** objectSlot);
    virtual void  addStaticRoot(TObject** pointer);
    virtual void  removeStaticRoot(TObject** pointer);
    virtual bool  isInStaticHeap(void* location);

    // External pointer handling
    virtual void  registerExternalHeapPointer(object_ptr& pointer);
    virtual void  releaseExternalHeapPointer(object_ptr& pointer);

    // Returns amount of allocations that were done after last GC
    // May be used as a flag that GC had just took place
    virtual uint32_t allocsBeyondCollection() { return m_memoryInfo.allocationsCount; }

    virtual TMemoryManagerInfo getStat();
};

class GenerationalMemoryManager : public BakerMemoryManager
{
protected:
    uint32_t m_leftToRightCollections;
    uint32_t m_rightToLeftCollections;
    uint32_t m_rightCollectionDelay;

    void collectLeftToRight(bool fullCollect = false);
    void collectRightToLeft();
    bool checkThreshold();
    void moveYoungObjects();

    bool isInYoungHeap(void* location);
    void addCrossgenReference(TObject** pointer);
    void removeCrossgenReference(TObject** pointer);

    typedef std::list<TMovableObject**> TPointerList;
    typedef std::list<TMovableObject**>::iterator TPointerIterator;
    TPointerList m_crossGenerationalReferences;
public:
    GenerationalMemoryManager() : BakerMemoryManager(),
        m_leftToRightCollections(0), m_rightToLeftCollections(0), m_rightCollectionDelay(0) { }
    virtual ~GenerationalMemoryManager();

    virtual bool checkRoot(TObject* value, TObject** objectSlot);
    virtual void collectGarbage();
    virtual TMemoryManagerInfo getStat();
};

class NonCollectMemoryManager : public IMemoryManager
{
protected:
    TMemoryManagerInfo m_memoryInfo;
    std::auto_ptr<IGCLogger> m_gcLogger;

    size_t    m_heapSize;
    uint8_t*  m_heapBase;
    uint8_t*  m_heapPointer;

    std::vector<void*> m_usedHeaps;

    size_t    m_staticHeapSize;
    uint8_t*  m_staticHeapBase;
    uint8_t*  m_staticHeapPointer;

    void growHeap();
public:
    NonCollectMemoryManager();
    virtual ~NonCollectMemoryManager();

    virtual bool  initializeHeap(size_t heapSize, size_t maxHeapSize = 0);
    virtual bool  initializeStaticHeap(size_t staticHeapSize);
    virtual void* allocate(size_t requestedSize, bool* gcOccured = 0);
    virtual void* staticAllocate(size_t requestedSize);
    virtual bool  isInStaticHeap(void* location);

    virtual void  collectGarbage() {}
    virtual void  addStaticRoot(TObject** /*pointer*/) {}
    virtual void  removeStaticRoot(TObject** /*pointer*/) {}
    virtual void  registerExternalPointer(TObject** /*pointer*/) {}
    virtual void  releaseExternalPointer(TObject** /*pointer*/) {}
    virtual void  registerExternalHeapPointer(object_ptr& /*pointer*/) {}
    virtual void  releaseExternalHeapPointer(object_ptr& /*pointer*/) {}
    virtual bool  checkRoot(TObject* /*value*/, TObject** /*objectSlot*/) { return false; }
    virtual uint32_t allocsBeyondCollection() { return 0; }
    virtual TMemoryManagerInfo getStat();
};

class LLVMMemoryManager : public BakerMemoryManager {
protected:
    virtual void moveObjects();

public:
    struct TFrameMap {
        int32_t numRoots;
        int32_t numMeta;
        const void* meta[0];
    };

    struct TStackEntry {
        TStackEntry* next;
        const TFrameMap* map;
        void* roots[0];
    };

    struct TMetaInfo {
        bool isStackObject : 1;
    };

    LLVMMemoryManager();
    virtual ~LLVMMemoryManager();
};

extern "C" { extern LLVMMemoryManager::TStackEntry* llvm_gc_root_chain; }

class Image
{
private:
    std::vector<TObject*> m_indirects;
    std::ifstream m_inputStream;

    enum TImageRecordType {
        invalidObject = 0,
        ordinaryObject,
        inlineInteger,  // inline 32 bit integer in network byte order
        byteObject,     //
        previousObject, // link to previously loaded object
        nilObject       // uninitialized (nil) field
    };

    uint32_t readWord();
    TObject* readObject();
    template<typename ResultType>
    ResultType* readObject() { return static_cast<ResultType*>(readObject()); }

    IMemoryManager* m_memoryManager;
public:
    Image(IMemoryManager* manager)
        : m_memoryManager(manager)
    { }

    bool     loadImage(const std::string& fileName);
    void     storeImage(const char* fileName);

    template<typename N> TObject* getGlobal(const N* name) const;
    template<typename T, typename N> T* getGlobal(const N* name) const { return static_cast<T*>(getGlobal(name)); }

    class ImageWriter;
    // GLobal VM objects
};

struct TGlobals {
    TObject* nilObject;
    TObject* trueObject;
    TObject* falseObject;
    TClass*  smallIntClass;
    TClass*  arrayClass;
    TClass*  blockClass;
    TClass*  contextClass;
    TClass*  stringClass;
    TDictionary* globalsObject;
    TMethod* initialMethod;
    TObject* binaryMessages[3];
    TClass*  integerClass;
    TSymbol* badMethodSymbol;
};

extern TGlobals globals;

class Image::ImageWriter
{
private:
    std::vector<TObject*> m_writtenObjects; //used to link objects together with type 'previousObject'
    TGlobals              m_globals;

    TImageRecordType getObjectType(TObject* object) const;
    int              getPreviousObjectIndex(TObject* object) const;
    void             writeWord(std::ofstream& os, uint32_t word);
    void             writeObject(std::ofstream& os, TObject* object);
public:
    ImageWriter();
    ImageWriter& setGlobals(const TGlobals& globals);
    void writeTo(const char* fileName);
};

#endif
