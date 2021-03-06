/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2013 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/jit/translator-runtime.h"

#include "hphp/runtime/ext/ext_function.h"
#include "hphp/runtime/vm/member-operations.h"
#include "hphp/runtime/vm/type-constraint.h"
#include "hphp/runtime/vm/jit/translator-inline.h"

namespace HPHP { namespace Transl {

ArrayData* addElemIntKeyHelper(ArrayData* ad,
                               int64_t key,
                               TypedValue value) {
  // this does not re-enter
  // set will decRef any old value that may have been overwritten
  // if appropriate
  ArrayData* retval = ad->set(key, tvAsCVarRef(&value),
                              ad->getCount() > 1);
  // TODO Task #1970153: It would be great if there were set()
  // methods that didn't bump up the refcount so that we didn't
  // have to decrement it here
  tvRefcountedDecRef(&value);
  return arrayRefShuffle<false>(ad, retval, nullptr);
}

ArrayData* addElemStringKeyHelper(ArrayData* ad,
                                  StringData* key,
                                  TypedValue value) {
  // this does not re-enter
  bool copy = ad->getCount() > 1;
  // set will decRef any old value that may have been overwritten
  // if appropriate
  int64_t intkey;
  ArrayData* retval = UNLIKELY(key->isStrictlyInteger(intkey)) ?
                      ad->set(intkey, tvAsCVarRef(&value), copy) :
                      ad->set(key, tvAsCVarRef(&value), copy);
  // TODO Task #1970153: It would be great if there were set()
  // methods that didn't bump up the refcount so that we didn't
  // have to decrement it here
  tvRefcountedDecRef(&value);
  return arrayRefShuffle<false>(ad, retval, nullptr);
}

ArrayData* array_add(ArrayData* a1, ArrayData* a2) {
  if (!a2->empty()) {
    if (a1->empty()) {
      decRefArr(a1);
      return a2;
    }
    if (a1 != a2) {
      ArrayData *escalated = a1->plus(a2, a1->getCount() > 1);
      if (escalated != a1) {
        escalated->incRefCount();
        decRefArr(a2);
        decRefArr(a1);
        return escalated;
      }
    }
  }
  decRefArr(a2);
  return a1;
}

HOT_FUNC_VM void setNewElem(TypedValue* base, Cell val) {
  SetNewElem<false>(base, &val);
}

HOT_FUNC_VM void setNewElemArray(TypedValue* base, Cell val) {
  SetNewElemArray(base, &val);
}

void bindNewElemIR(TypedValue* base, RefData* val, MInstrState* mis) {
  base = NewElem(mis->tvScratch, mis->tvRef, base);
  if (!(base == &mis->tvScratch && base->m_type == KindOfUninit)) {
    tvBindRef(val, base);
  }
}

// TODO: Kill this #2031980
HOT_FUNC_VM RefData* box_value(TypedValue tv) {
  return tvBoxHelper(tv.m_type, tv.m_data.num);
}

ArrayData* convCellToArrHelper(TypedValue tv) {
  // Note: the call sites of this function all assume that
  // no user code will run and no recoverable exceptions will
  // occur while running this code. This seems trivially true
  // in all cases but converting objects to arrays. It also
  // seems true for that case as well, since the resulting array
  // is essentially metadata for the object. If that is not true,
  // you might end up looking at this code in a debugger and now
  // you know why.
  tvCastToArrayInPlace(&tv); // consumes a ref on counted values
  return tv.m_data.parr;
}

int64_t convArrToBoolHelper(const ArrayData* a) {
  return a->size() != 0;
}

int64_t convObjToBoolHelper(const ObjectData* o) {
  return o->o_toBoolean();
}

int64_t convArrToDblHelper(ArrayData* a) {
  return reinterpretDblAsInt(a->empty() ? 0 : 1);
}

int64_t convStrToDblHelper(const StringData* s) {
  return reinterpretDblAsInt(s->toDouble());
}

int64_t convCellToDblHelper(TypedValue tv) {
  return reinterpretDblAsInt(tvCastToDouble(&tv));
}

int64_t convArrToIntHelper(ArrayData* a) {
  return a->empty() ? 0 : 1;
}

ObjectData* convCellToObjHelper(TypedValue tv) {
  // Note: the call sites of this function all assume that
  // no user code will run and no recoverable exceptions will
  // occur while running this code. This seems trivially true
  // in all cases but converting arrays to objects. It also
  // seems true for that case as well, since the source array
  // is essentially metadata for the object. If that is not true,
  // you might end up looking at this code in a debugger and now
  // you know why.
  tvCastToObjectInPlace(&tv); // consumes a ref on counted values
  return tv.m_data.pobj;
}

StringData* convDblToStrHelper(int64_t i) {
  double d = reinterpretIntAsDbl(i);
  auto r = buildStringData(d);
  r->incRefCount();
  return r;
}

StringData* convIntToStrHelper(int64_t i) {
  auto r = buildStringData(i);
  r->incRefCount();
  return r;
}

StringData* convObjToStrHelper(ObjectData* o) {
  auto s = o->invokeToString();
  auto r = s.get();
  decRefObj(o);
  if (!r->isStatic()) r->incRefCount();
  return r;
}

StringData* convResToStrHelper(ResourceData* o) {
  auto s = o->o_toString();
  auto r = s.get();
  decRefRes(o);
  if (!r->isStatic()) r->incRefCount();
  return r;
}

const StaticString
  s_empty(""),
  s_1("1"),
  s_Array("Array");

StringData* convCellToStrHelper(TypedValue tv) {
  switch (tv.m_type) {
  case KindOfUninit:
  case KindOfNull:     return s_empty.get();
  case KindOfBoolean:  return tv.m_data.num ? s_1.get() : s_empty.get();
  case KindOfInt64:    return convIntToStrHelper(tv.m_data.num);
  case KindOfDouble:   return convDblToStrHelper(tv.m_data.num);
  case KindOfStaticString:
  case KindOfString:   // No incref, since there is also a logical decref of
                       // our argument.
                       return tv.m_data.pstr;
  case KindOfArray:    tvDecRefArr(&tv); return s_Array.get();
  case KindOfObject:   return convObjToStrHelper(tv.m_data.pobj);
  case KindOfResource: return convResToStrHelper(tv.m_data.pres);
  default:             not_reached();
  }
}

void raisePropertyOnNonObject() {
  raise_warning("Cannot access property on non-object");
}

void raiseUndefProp(ObjectData* base, const StringData* name) {
  base->raiseUndefProp(name);
}

void raise_error_sd(const StringData *msg) {
  raise_error("%s", msg->data());
}

void VerifyParamTypeFail(int paramNum) {
  VMRegAnchor _;
  const ActRec* ar = liveFrame();
  const Func* func = ar->m_func;
  const TypeConstraint& tc = func->params()[paramNum].typeConstraint();
  TypedValue* tv = frame_local(ar, paramNum);
  assert(!tc.check(tv, func));
  tc.verifyFail(func, paramNum, tv);
}

void VerifyParamTypeCallable(TypedValue value, int param) {
  if (UNLIKELY(!f_is_callable(tvAsCVarRef(&value)))) {
    VerifyParamTypeFail(param);
  }
}

HOT_FUNC_VM
void VerifyParamTypeSlow(const Class* cls,
                         const Class* constraint,
                         int param,
                         const TypeConstraint* expected) {
  if (LIKELY(constraint && cls->classof(constraint))) {
    return;
  }

  // Check a typedef for a class.  We interp'd if the param wasn't an
  // object, so if it's a typedef for something non-objecty we're
  // failing anyway.
  if (auto namedEntity = expected->namedEntity()) {
    auto def = namedEntity->getCachedTypedef();
    if (UNLIKELY(!def)) {
      VMRegAnchor _;
      String nameStr(const_cast<StringData*>(expected->typeName()));
      if (AutoloadHandler::s_instance->autoloadType(nameStr)) {
        def = namedEntity->getCachedTypedef();
      }
    }
    if (def) {
      // There's no need to handle nullable typedefs specially here:
      // we already know we're checking a non-null object with the
      // class `cls'.  We do however need to check for typedefs to
      // mixed.
      if (def->kind == KindOfObject) {
        constraint = def->klass;
        if (constraint && cls->classof(constraint)) return;
      } else if (def->kind == KindOfAny) {
        return;
      }
    }
  }

  VerifyParamTypeFail(param);
}

template<bool useTargetCache>
RefData* staticLocInitImpl(StringData* name, ActRec* fp, TypedValue val,
                           TargetCache::CacheHandle ch) {
  assert(useTargetCache == (bool)ch);
  HphpArray* map;
  if (useTargetCache) {
    // If we have a cache handle, we know the current func isn't a
    // closure or generator closure so we can directly grab its static
    // locals map.
    const Func* func = fp->m_func;
    assert(!(func->isClosureBody() || func->isGeneratorFromClosure()));
    map = func->getStaticLocals();
  } else {
    map = get_static_locals(fp);
  }

  TypedValue *mapVal = map->nvGet(name);
  if (!mapVal) {
    map->set(name, tvAsCVarRef(&val), false);
    mapVal = map->nvGet(name);
  }
  if (mapVal->m_type != KindOfRef) {
    tvBox(mapVal);
  }
  assert(mapVal->m_type == KindOfRef);
  RefData* ret = mapVal->m_data.pref;
  if (useTargetCache) {
    *TargetCache::handleToPtr<RefData*>(ch) = ret;
  }
  ret->incRefCount();
  return ret;
}

RefData* staticLocInit(StringData* name, ActRec* fp, TypedValue val) {
  return staticLocInitImpl<false>(name, fp, val, 0);
}

RefData* staticLocInitCached(StringData* name, ActRec* fp, TypedValue val,
                             TargetCache::CacheHandle ch) {
  return staticLocInitImpl<true>(name, fp, val, ch);
}

HOT_FUNC_VM
bool instanceOfHelper(const Class* objClass,
                      const Class* testClass) {
  return testClass && objClass->classof(testClass);
}

} }
