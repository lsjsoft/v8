// Copyright 2011 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/contexts.h"

#include "src/ast/scopeinfo.h"
#include "src/bootstrapper.h"
#include "src/debug/debug.h"
#include "src/isolate-inl.h"

namespace v8 {
namespace internal {


Handle<ScriptContextTable> ScriptContextTable::Extend(
    Handle<ScriptContextTable> table, Handle<Context> script_context) {
  Handle<ScriptContextTable> result;
  int used = table->used();
  int length = table->length();
  CHECK(used >= 0 && length > 0 && used < length);
  if (used + kFirstContextSlot == length) {
    CHECK(length < Smi::kMaxValue / 2);
    Isolate* isolate = table->GetIsolate();
    Handle<FixedArray> copy =
        isolate->factory()->CopyFixedArrayAndGrow(table, length);
    copy->set_map(isolate->heap()->script_context_table_map());
    result = Handle<ScriptContextTable>::cast(copy);
  } else {
    result = table;
  }
  result->set_used(used + 1);

  DCHECK(script_context->IsScriptContext());
  result->set(used + kFirstContextSlot, *script_context);
  return result;
}


bool ScriptContextTable::Lookup(Handle<ScriptContextTable> table,
                                Handle<String> name, LookupResult* result) {
  for (int i = 0; i < table->used(); i++) {
    Handle<Context> context = GetContext(table, i);
    DCHECK(context->IsScriptContext());
    Handle<ScopeInfo> scope_info(context->scope_info());
    int slot_index = ScopeInfo::ContextSlotIndex(
        scope_info, name, &result->mode, &result->init_flag,
        &result->maybe_assigned_flag);

    if (slot_index >= 0) {
      result->context_index = i;
      result->slot_index = slot_index;
      return true;
    }
  }
  return false;
}


bool Context::is_declaration_context() {
  if (IsFunctionContext() || IsNativeContext() || IsScriptContext()) {
    return true;
  }
  if (!IsBlockContext()) return false;
  Object* ext = extension();
  // If we have the special extension, we immediately know it must be a
  // declaration scope. That's just a small performance shortcut.
  return ext->IsSloppyBlockWithEvalContextExtension()
      || ScopeInfo::cast(ext)->is_declaration_scope();
}


Context* Context::declaration_context() {
  Context* current = this;
  while (!current->is_declaration_context()) {
    current = current->previous();
    DCHECK(current->closure() == closure());
  }
  return current;
}


JSObject* Context::extension_object() {
  DCHECK(IsNativeContext() || IsFunctionContext() || IsBlockContext());
  Object* object = extension();
  if (object == nullptr) return nullptr;
  if (IsBlockContext()) {
    if (!object->IsSloppyBlockWithEvalContextExtension()) return nullptr;
    object = SloppyBlockWithEvalContextExtension::cast(object)->extension();
  }
  DCHECK(object->IsJSContextExtensionObject() ||
         (IsNativeContext() && object->IsJSGlobalObject()));
  return JSObject::cast(object);
}


JSReceiver* Context::extension_receiver() {
  DCHECK(IsNativeContext() || IsWithContext() ||
         IsFunctionContext() || IsBlockContext());
  return IsWithContext() ? JSReceiver::cast(extension()) : extension_object();
}


ScopeInfo* Context::scope_info() {
  DCHECK(IsModuleContext() || IsScriptContext() || IsBlockContext());
  Object* object = extension();
  if (object->IsSloppyBlockWithEvalContextExtension()) {
    DCHECK(IsBlockContext());
    object = SloppyBlockWithEvalContextExtension::cast(object)->scope_info();
  }
  return ScopeInfo::cast(object);
}


String* Context::catch_name() {
  DCHECK(IsCatchContext());
  return String::cast(extension());
}


JSGlobalObject* Context::global_object() {
  return JSGlobalObject::cast(native_context()->extension());
}


Context* Context::script_context() {
  Context* current = this;
  while (!current->IsScriptContext()) {
    current = current->previous();
  }
  return current;
}


JSObject* Context::global_proxy() {
  return native_context()->global_proxy_object();
}


void Context::set_global_proxy(JSObject* object) {
  native_context()->set_global_proxy_object(object);
}


/**
 * Lookups a property in an object environment, taking the unscopables into
 * account. This is used For HasBinding spec algorithms for ObjectEnvironment.
 */
static Maybe<PropertyAttributes> UnscopableLookup(LookupIterator* it) {
  Isolate* isolate = it->isolate();

  Maybe<PropertyAttributes> attrs = JSReceiver::GetPropertyAttributes(it);
  DCHECK(attrs.IsJust() || isolate->has_pending_exception());
  if (!attrs.IsJust() || attrs.FromJust() == ABSENT) return attrs;

  Handle<Symbol> unscopables_symbol = isolate->factory()->unscopables_symbol();
  Handle<Object> receiver = it->GetReceiver();
  Handle<Object> unscopables;
  MaybeHandle<Object> maybe_unscopables =
      Object::GetProperty(receiver, unscopables_symbol);
  if (!maybe_unscopables.ToHandle(&unscopables)) {
    return Nothing<PropertyAttributes>();
  }
  if (!unscopables->IsSpecObject()) return attrs;
  Handle<Object> blacklist;
  MaybeHandle<Object> maybe_blacklist =
      Object::GetProperty(unscopables, it->name());
  if (!maybe_blacklist.ToHandle(&blacklist)) {
    DCHECK(isolate->has_pending_exception());
    return Nothing<PropertyAttributes>();
  }
  return blacklist->BooleanValue() ? Just(ABSENT) : attrs;
}

static void GetAttributesAndBindingFlags(VariableMode mode,
                                         InitializationFlag init_flag,
                                         PropertyAttributes* attributes,
                                         BindingFlags* binding_flags) {
  switch (mode) {
    case VAR:
      *attributes = NONE;
      *binding_flags = MUTABLE_IS_INITIALIZED;
      break;
    case LET:
      *attributes = NONE;
      *binding_flags = (init_flag == kNeedsInitialization)
                           ? MUTABLE_CHECK_INITIALIZED
                           : MUTABLE_IS_INITIALIZED;
      break;
    case CONST_LEGACY:
      *attributes = READ_ONLY;
      *binding_flags = (init_flag == kNeedsInitialization)
                           ? IMMUTABLE_CHECK_INITIALIZED
                           : IMMUTABLE_IS_INITIALIZED;
      break;
    case CONST:
      *attributes = READ_ONLY;
      *binding_flags = (init_flag == kNeedsInitialization)
                           ? IMMUTABLE_CHECK_INITIALIZED_HARMONY
                           : IMMUTABLE_IS_INITIALIZED_HARMONY;
      break;
    case IMPORT:
      // TODO(ES6)
      UNREACHABLE();
      break;
    case DYNAMIC:
    case DYNAMIC_GLOBAL:
    case DYNAMIC_LOCAL:
    case TEMPORARY:
      // Note: Fixed context slots are statically allocated by the compiler.
      // Statically allocated variables always have a statically known mode,
      // which is the mode with which they were declared when added to the
      // scope. Thus, the DYNAMIC mode (which corresponds to dynamically
      // declared variables that were introduced through declaration nodes)
      // must not appear here.
      UNREACHABLE();
      break;
  }
}


Handle<Object> Context::Lookup(Handle<String> name,
                               ContextLookupFlags flags,
                               int* index,
                               PropertyAttributes* attributes,
                               BindingFlags* binding_flags) {
  Isolate* isolate = GetIsolate();
  Handle<Context> context(this, isolate);

  bool follow_context_chain = (flags & FOLLOW_CONTEXT_CHAIN) != 0;
  *index = kNotFound;
  *attributes = ABSENT;
  *binding_flags = MISSING_BINDING;

  if (FLAG_trace_contexts) {
    PrintF("Context::Lookup(");
    name->ShortPrint();
    PrintF(")\n");
  }

  do {
    if (FLAG_trace_contexts) {
      PrintF(" - looking in context %p", reinterpret_cast<void*>(*context));
      if (context->IsScriptContext()) PrintF(" (script context)");
      if (context->IsNativeContext()) PrintF(" (native context)");
      PrintF("\n");
    }

    // 1. Check global objects, subjects of with, and extension objects.
    if ((context->IsNativeContext() ||
         (context->IsWithContext() && ((flags & SKIP_WITH_CONTEXT) == 0)) ||
         context->IsFunctionContext() || context->IsBlockContext()) &&
        context->extension_receiver() != nullptr) {
      Handle<JSReceiver> object(context->extension_receiver());

      if (context->IsNativeContext()) {
        if (FLAG_trace_contexts) {
          PrintF(" - trying other script contexts\n");
        }
        // Try other script contexts.
        Handle<ScriptContextTable> script_contexts(
            context->global_object()->native_context()->script_context_table());
        ScriptContextTable::LookupResult r;
        if (ScriptContextTable::Lookup(script_contexts, name, &r)) {
          if (FLAG_trace_contexts) {
            Handle<Context> c = ScriptContextTable::GetContext(script_contexts,
                                                               r.context_index);
            PrintF("=> found property in script context %d: %p\n",
                   r.context_index, reinterpret_cast<void*>(*c));
          }
          *index = r.slot_index;
          GetAttributesAndBindingFlags(r.mode, r.init_flag, attributes,
                                       binding_flags);
          return ScriptContextTable::GetContext(script_contexts,
                                                r.context_index);
        }
      }

      // Context extension objects needs to behave as if they have no
      // prototype.  So even if we want to follow prototype chains, we need
      // to only do a local lookup for context extension objects.
      Maybe<PropertyAttributes> maybe = Nothing<PropertyAttributes>();
      if ((flags & FOLLOW_PROTOTYPE_CHAIN) == 0 ||
          object->IsJSContextExtensionObject()) {
        maybe = JSReceiver::GetOwnPropertyAttributes(object, name);
      } else if (context->IsWithContext()) {
        // A with context will never bind "this".
        if (name->Equals(*isolate->factory()->this_string())) {
          maybe = Just(ABSENT);
        } else {
          LookupIterator it(object, name);
          maybe = UnscopableLookup(&it);
        }
      } else {
        maybe = JSReceiver::GetPropertyAttributes(object, name);
      }

      if (!maybe.IsJust()) return Handle<Object>();
      DCHECK(!isolate->has_pending_exception());
      *attributes = maybe.FromJust();

      if (maybe.FromJust() != ABSENT) {
        if (FLAG_trace_contexts) {
          PrintF("=> found property in context object %p\n",
                 reinterpret_cast<void*>(*object));
        }
        return object;
      }
    }

    // 2. Check the context proper if it has slots.
    if (context->IsFunctionContext() || context->IsBlockContext() ||
        context->IsScriptContext()) {
      // Use serialized scope information of functions and blocks to search
      // for the context index.
      Handle<ScopeInfo> scope_info(context->IsFunctionContext()
          ? context->closure()->shared()->scope_info()
          : context->scope_info());
      VariableMode mode;
      InitializationFlag init_flag;
      // TODO(sigurds) Figure out whether maybe_assigned_flag should
      // be used to compute binding_flags.
      MaybeAssignedFlag maybe_assigned_flag;
      int slot_index = ScopeInfo::ContextSlotIndex(
          scope_info, name, &mode, &init_flag, &maybe_assigned_flag);
      DCHECK(slot_index < 0 || slot_index >= MIN_CONTEXT_SLOTS);
      if (slot_index >= 0) {
        if (FLAG_trace_contexts) {
          PrintF("=> found local in context slot %d (mode = %d)\n",
                 slot_index, mode);
        }
        *index = slot_index;
        GetAttributesAndBindingFlags(mode, init_flag, attributes,
                                     binding_flags);
        return context;
      }

      // Check the slot corresponding to the intermediate context holding
      // only the function name variable.
      if (follow_context_chain && context->IsFunctionContext()) {
        VariableMode mode;
        int function_index = scope_info->FunctionContextSlotIndex(*name, &mode);
        if (function_index >= 0) {
          if (FLAG_trace_contexts) {
            PrintF("=> found intermediate function in context slot %d\n",
                   function_index);
          }
          *index = function_index;
          *attributes = READ_ONLY;
          DCHECK(mode == CONST_LEGACY || mode == CONST);
          *binding_flags = (mode == CONST_LEGACY)
              ? IMMUTABLE_IS_INITIALIZED : IMMUTABLE_IS_INITIALIZED_HARMONY;
          return context;
        }
      }

    } else if (context->IsCatchContext()) {
      // Catch contexts have the variable name in the extension slot.
      if (String::Equals(name, handle(context->catch_name()))) {
        if (FLAG_trace_contexts) {
          PrintF("=> found in catch context\n");
        }
        *index = Context::THROWN_OBJECT_INDEX;
        *attributes = NONE;
        *binding_flags = MUTABLE_IS_INITIALIZED;
        return context;
      }
    }

    // 3. Prepare to continue with the previous (next outermost) context.
    if (context->IsNativeContext() ||
        ((flags & STOP_AT_DECLARATION_SCOPE) != 0 &&
         context->is_declaration_context())) {
      follow_context_chain = false;
    } else {
      context = Handle<Context>(context->previous(), isolate);
    }
  } while (follow_context_chain);

  if (FLAG_trace_contexts) {
    PrintF("=> no property/slot found\n");
  }
  return Handle<Object>::null();
}


void Context::InitializeGlobalSlots() {
  DCHECK(IsScriptContext());
  DisallowHeapAllocation no_gc;

  ScopeInfo* scope_info = this->scope_info();

  int context_globals = scope_info->ContextGlobalCount();
  if (context_globals > 0) {
    PropertyCell* empty_cell = GetHeap()->empty_property_cell();

    int context_locals = scope_info->ContextLocalCount();
    int index = Context::MIN_CONTEXT_SLOTS + context_locals;
    for (int i = 0; i < context_globals; i++) {
      set(index++, empty_cell);
    }
  }
}


void Context::AddOptimizedFunction(JSFunction* function) {
  DCHECK(IsNativeContext());
#ifdef ENABLE_SLOW_DCHECKS
  if (FLAG_enable_slow_asserts) {
    Object* element = get(OPTIMIZED_FUNCTIONS_LIST);
    while (!element->IsUndefined()) {
      CHECK(element != function);
      element = JSFunction::cast(element)->next_function_link();
    }
  }

  // Check that the context belongs to the weak native contexts list.
  bool found = false;
  Object* context = GetHeap()->native_contexts_list();
  while (!context->IsUndefined()) {
    if (context == this) {
      found = true;
      break;
    }
    context = Context::cast(context)->get(Context::NEXT_CONTEXT_LINK);
  }
  CHECK(found);
#endif

  // If the function link field is already used then the function was
  // enqueued as a code flushing candidate and we remove it now.
  if (!function->next_function_link()->IsUndefined()) {
    CodeFlusher* flusher = GetHeap()->mark_compact_collector()->code_flusher();
    flusher->EvictCandidate(function);
  }

  DCHECK(function->next_function_link()->IsUndefined());

  function->set_next_function_link(get(OPTIMIZED_FUNCTIONS_LIST),
                                   UPDATE_WEAK_WRITE_BARRIER);
  set(OPTIMIZED_FUNCTIONS_LIST, function, UPDATE_WEAK_WRITE_BARRIER);
}


void Context::RemoveOptimizedFunction(JSFunction* function) {
  DCHECK(IsNativeContext());
  Object* element = get(OPTIMIZED_FUNCTIONS_LIST);
  JSFunction* prev = NULL;
  while (!element->IsUndefined()) {
    JSFunction* element_function = JSFunction::cast(element);
    DCHECK(element_function->next_function_link()->IsUndefined() ||
           element_function->next_function_link()->IsJSFunction());
    if (element_function == function) {
      if (prev == NULL) {
        set(OPTIMIZED_FUNCTIONS_LIST, element_function->next_function_link(),
            UPDATE_WEAK_WRITE_BARRIER);
      } else {
        prev->set_next_function_link(element_function->next_function_link(),
                                     UPDATE_WEAK_WRITE_BARRIER);
      }
      element_function->set_next_function_link(GetHeap()->undefined_value(),
                                               UPDATE_WEAK_WRITE_BARRIER);
      return;
    }
    prev = element_function;
    element = element_function->next_function_link();
  }
  UNREACHABLE();
}


void Context::SetOptimizedFunctionsListHead(Object* head) {
  DCHECK(IsNativeContext());
  set(OPTIMIZED_FUNCTIONS_LIST, head, UPDATE_WEAK_WRITE_BARRIER);
}


Object* Context::OptimizedFunctionsListHead() {
  DCHECK(IsNativeContext());
  return get(OPTIMIZED_FUNCTIONS_LIST);
}


void Context::AddOptimizedCode(Code* code) {
  DCHECK(IsNativeContext());
  DCHECK(code->kind() == Code::OPTIMIZED_FUNCTION);
  DCHECK(code->next_code_link()->IsUndefined());
  code->set_next_code_link(get(OPTIMIZED_CODE_LIST));
  set(OPTIMIZED_CODE_LIST, code, UPDATE_WEAK_WRITE_BARRIER);
}


void Context::SetOptimizedCodeListHead(Object* head) {
  DCHECK(IsNativeContext());
  set(OPTIMIZED_CODE_LIST, head, UPDATE_WEAK_WRITE_BARRIER);
}


Object* Context::OptimizedCodeListHead() {
  DCHECK(IsNativeContext());
  return get(OPTIMIZED_CODE_LIST);
}


void Context::SetDeoptimizedCodeListHead(Object* head) {
  DCHECK(IsNativeContext());
  set(DEOPTIMIZED_CODE_LIST, head, UPDATE_WEAK_WRITE_BARRIER);
}


Object* Context::DeoptimizedCodeListHead() {
  DCHECK(IsNativeContext());
  return get(DEOPTIMIZED_CODE_LIST);
}


Handle<Object> Context::ErrorMessageForCodeGenerationFromStrings() {
  Isolate* isolate = GetIsolate();
  Handle<Object> result(error_message_for_code_gen_from_strings(), isolate);
  if (!result->IsUndefined()) return result;
  return isolate->factory()->NewStringFromStaticChars(
      "Code generation from strings disallowed for this context");
}


#define COMPARE_NAME(index, type, name) \
  if (string->IsOneByteEqualTo(STATIC_CHAR_VECTOR(#name))) return index;

int Context::ImportedFieldIndexForName(Handle<String> string) {
  NATIVE_CONTEXT_IMPORTED_FIELDS(COMPARE_NAME)
  return kNotFound;
}


int Context::IntrinsicIndexForName(Handle<String> string) {
  NATIVE_CONTEXT_INTRINSIC_FUNCTIONS(COMPARE_NAME);
  return kNotFound;
}

#undef COMPARE_NAME


bool Context::IsJSBuiltin(Handle<Context> native_context,
                          Handle<JSFunction> function) {
#define COMPARE_FUNCTION(index, type, name) \
  if (*function == native_context->get(index)) return true;
  NATIVE_CONTEXT_JS_BUILTINS(COMPARE_FUNCTION);
#undef COMPARE_FUNCTION
  return false;
}


#ifdef DEBUG

bool Context::IsBootstrappingOrNativeContext(Isolate* isolate, Object* object) {
  // During bootstrapping we allow all objects to pass as global
  // objects. This is necessary to fix circular dependencies.
  return isolate->heap()->gc_state() != Heap::NOT_IN_GC ||
         isolate->bootstrapper()->IsActive() || object->IsNativeContext();
}


bool Context::IsBootstrappingOrValidParentContext(
    Object* object, Context* child) {
  // During bootstrapping we allow all objects to pass as
  // contexts. This is necessary to fix circular dependencies.
  if (child->GetIsolate()->bootstrapper()->IsActive()) return true;
  if (!object->IsContext()) return false;
  Context* context = Context::cast(object);
  return context->IsNativeContext() || context->IsScriptContext() ||
         context->IsModuleContext() || !child->IsModuleContext();
}

#endif


void Context::IncrementErrorsThrown() {
  DCHECK(IsNativeContext());

  int previous_value = errors_thrown()->value();
  set_errors_thrown(Smi::FromInt(previous_value + 1));
}


int Context::GetErrorsThrown() { return errors_thrown()->value(); }

}  // namespace internal
}  // namespace v8
