/**************************************************************************/
/*  gdtype.cpp                                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "gdtype.h"

#include "core/object/method_bind.h"
#include "core/os/memory.h"
#include "core/os/thread.h"

GDType::GDType(const GDType *p_super_type, StringName p_name) :
		super_type(p_super_type), name(std::move(p_name)) {
	name_hierarchy.push_back(name);

	if (super_type) {
		for (const StringName &ancestor_name : super_type->name_hierarchy) {
			name_hierarchy.push_back(ancestor_name);
		}
	}
}

GDType::~GDType() {
	for (const KeyValue<StringName, const EnumInfo *> &kv : self_enum_map) {
		memdelete(const_cast<EnumInfo *>(kv.value));
	}
	for (const KeyValue<StringName, const MethodInfo *> &kv : self_signal_map) {
		memdelete(const_cast<MethodInfo *>(kv.value));
	}
	for (const KeyValue<StringName, const MethodBind *> &kv : self_method_map) {
		memdelete(const_cast<MethodBind *>(kv.value));
	}
	for (const PropertyInfo *pinfo : self_property_list) {
		memdelete(const_cast<PropertyInfo *>(pinfo));
	}
	for (const KeyValue<StringName, const PropertySetGet *> &kv : self_property_setget) {
		memdelete(const_cast<PropertySetGet *>(kv.value));
	}
#ifdef TOOLS_ENABLED
	for (const KeyValue<StringName, List<StringName> *> &kv : self_linked_properties) {
		memdelete(const_cast<List<StringName> *>(kv.value));
	}
#endif
}

void GDType::initialize() {
	ERR_FAIL_COND(init_state != InitState::UNINITIALIZED);

	if (super_type) {
		// Now that a subtype is registered, the supertype cannot change anymore.
		// Otherwise, our caches would become invalid.
		// This shouldn't be a problem, since classes should register all their
		// parts in _bind_methods, which is called on registration.
		super_type->init_state = InitState::FINALIZED;

		constant_map = super_type->constant_map;
		enum_map = super_type->enum_map;
		signal_map = super_type->signal_map;
		method_map = super_type->method_map;
		property_list = super_type->property_list;
		property_map = super_type->property_map;
		property_setget = super_type->property_setget;
#ifdef DEBUG_ENABLED
		methods_in_properties = super_type->methods_in_properties;
#endif
#ifdef TOOLS_ENABLED
		linked_properties = super_type->linked_properties;
#endif
	}

	init_state = InitState::MUTABLE;
}

void GDType::bind_integer_constant(const StringName &p_enum, const StringName &p_name, int64_t p_constant, bool p_is_bitfield) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ERR_FAIL_COND(init_state != InitState::MUTABLE);
	ERR_FAIL_COND_MSG(self_constant_map.has(p_name), vformat("Class '%s' already has constant '%s'.", String(name), String(p_name)));

	constant_map[p_name] = p_constant;
	self_constant_map[p_name] = p_constant;

	String enum_name = p_enum;
	if (!enum_name.is_empty()) {
		if (enum_name.contains_char('.')) {
			enum_name = enum_name.get_slicec('.', 1);
		}

		const EnumInfo **_enum_info = self_enum_map.getptr(enum_name);

		if (_enum_info != nullptr) {
			EnumInfo *enum_info = const_cast<EnumInfo *>(*_enum_info);
			enum_info->values.insert(p_name, p_constant);
			enum_info->is_bitfield = p_is_bitfield;
		} else {
			EnumInfo *enum_info = memnew(EnumInfo);
			enum_info->name = enum_name;
			enum_info->is_bitfield = p_is_bitfield;
			enum_info->values.insert(p_name, p_constant);
			self_enum_map[enum_name] = enum_info;
			enum_map[enum_name] = enum_info;
		}
	}
}

const GDType::EnumInfo *GDType::get_integer_constant_enum(const StringName &p_name, bool p_no_inheritance) const {
	for (const KeyValue<StringName, const EnumInfo *> &kv : get_enum_map(p_no_inheritance)) {
		if (kv.value->values.has(p_name)) {
			return kv.value;
		}
	}

	return nullptr;
}

void GDType::add_signal(MethodInfo p_signal) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ERR_FAIL_COND(init_state != InitState::MUTABLE);

	const StringName signal_name(p_signal.name);
	ERR_FAIL_COND_MSG(signal_map.has(signal_name), vformat("Class '%s' already has signal '%s'.", String(name), String(signal_name)));

	const MethodInfo *ptr = memnew(MethodInfo(std::move(p_signal)));

	signal_map[signal_name] = ptr;
	self_signal_map[signal_name] = ptr;
}

void GDType::free_method_binds() {
	for (KeyValue<StringName, const MethodBind *> &F : self_method_map) {
		memdelete(const_cast<MethodBind *>(F.value));
	}
}

bool GDType::bind_method(MethodBind *p_method) {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), false);
	ERR_FAIL_COND_V(init_state != InitState::MUTABLE, false);

	if (self_method_map.has(p_method->get_name())) {
		memdelete(p_method);
		ERR_FAIL_V_MSG(false, vformat("Method already bound '%s::%s'.", name, p_method->get_name()));
	}

	method_map[p_method->get_name()] = p_method;
	self_method_map[p_method->get_name()] = p_method;
	return true;
}

void GDType::set_method_flags(const StringName &p_method, int p_flags) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ERR_FAIL_COND(init_state != InitState::MUTABLE);

	(const_cast<MethodBind *>(self_method_map[p_method]))->set_hint_flags(p_flags);
}

void GDType::add_property_group(const String &p_name, const String &p_prefix, int p_indent_depth) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ERR_FAIL_COND(init_state != InitState::MUTABLE);
	String prefix = p_prefix;
	if (p_indent_depth > 0) {
		prefix = vformat("%s,%d", p_prefix, p_indent_depth);
	}

	PropertyInfo *pinfo = memnew(PropertyInfo(Variant::NIL, p_name, PROPERTY_HINT_NONE, prefix, PROPERTY_USAGE_GROUP));
	property_list.push_back(pinfo);
	self_property_list.push_back(pinfo);
}

void GDType::add_property_subgroup(const String &p_name, const String &p_prefix, int p_indent_depth) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ERR_FAIL_COND(init_state != InitState::MUTABLE);
	String prefix = p_prefix;
	if (p_indent_depth > 0) {
		prefix = vformat("%s,%d", p_prefix, p_indent_depth);
	}

	PropertyInfo *pinfo = memnew(PropertyInfo(Variant::NIL, p_name, PROPERTY_HINT_NONE, prefix, PROPERTY_USAGE_SUBGROUP));
	property_list.push_back(pinfo);
	self_property_list.push_back(pinfo);
}

void GDType::add_property_array_count(const String &p_label, const StringName &p_count_property, const StringName &p_count_setter, const StringName &p_count_getter, const String &p_array_element_prefix, uint32_t p_count_usage) {
	add_property(PropertyInfo(Variant::INT, p_count_property, PROPERTY_HINT_NONE, "", p_count_usage | PROPERTY_USAGE_ARRAY, vformat("%s,%s", p_label, p_array_element_prefix)), p_count_setter, p_count_getter);
}

void GDType::add_property_array(const StringName &p_path, const String &p_array_element_prefix) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ERR_FAIL_COND(init_state != InitState::MUTABLE);

	PropertyInfo *pinfo = memnew(PropertyInfo(Variant::NIL, p_path, PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_ARRAY, p_array_element_prefix));

	property_list.push_back(pinfo);
	self_property_list.push_back(pinfo);
}

void GDType::add_property(const PropertyInfo &p_pinfo, const StringName &p_setter, const StringName &p_getter, int p_index) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ERR_FAIL_COND(init_state != InitState::MUTABLE);

	const StringName property_name(p_pinfo.name);
	ERR_FAIL_COND_MSG(property_map.has(property_name), vformat("Class '%s' already has property '%s'.", String(name), String(property_name)));

	const MethodBind *mb_set = nullptr;
	if (p_setter) {
		mb_set = *method_map.getptr(p_setter);
#ifdef DEBUG_ENABLED

		ERR_FAIL_NULL_MSG(mb_set, vformat("Invalid setter '%s::%s' for property '%s'.", name, p_setter, p_pinfo.name));

		int exp_args = 1 + (p_index >= 0 ? 1 : 0);
		ERR_FAIL_COND_MSG(mb_set->get_argument_count() != exp_args, vformat("Invalid function for setter '%s::%s' for property '%s'.", name, p_setter, p_pinfo.name));
#endif // DEBUG_ENABLED
	}

	const MethodBind *mb_get = nullptr;
	if (p_getter) {
		mb_get = *method_map.getptr(p_getter);
#ifdef DEBUG_ENABLED

		ERR_FAIL_NULL_MSG(mb_get, vformat("Invalid getter '%s::%s' for property '%s'.", name, p_getter, p_pinfo.name));

		int exp_args = 0 + (p_index >= 0 ? 1 : 0);
		ERR_FAIL_COND_MSG(mb_get->get_argument_count() != exp_args, vformat("Invalid function for getter '%s::%s' for property '%s'.", name, p_getter, p_pinfo.name));
#endif // DEBUG_ENABLED
	}

#ifdef DEBUG_ENABLED
	ERR_FAIL_COND_MSG(self_property_setget.has(p_pinfo.name), vformat("Object '%s' already has property '%s'.", name, p_pinfo.name));
#endif // DEBUG_ENABLED

	const PropertyInfo *pinfo = memnew(PropertyInfo(p_pinfo));

	property_list.push_back(pinfo);
	self_property_list.push_back(pinfo);
	property_map[property_name] = pinfo;
	self_property_map[property_name] = pinfo;
#ifdef DEBUG_ENABLED
	// Used to filter out setters and getters in the editor (e.g. autocomplete) to not clutter menus. We only want to filter methods from properties that are easily available to users.
	if (p_index == -1 && !(p_pinfo.usage & PropertyUsageFlags::PROPERTY_USAGE_INTERNAL)) {
		if (mb_get) {
			methods_in_properties.insert(p_getter);
			self_methods_in_properties.insert(p_getter);
		}
		if (mb_set) {
			methods_in_properties.insert(p_setter);
			self_methods_in_properties.insert(p_setter);
		}
	}
#endif // DEBUG_ENABLED
	PropertySetGet *psg = memnew(PropertySetGet);
	psg->setter = p_setter;
	psg->getter = p_getter;
	psg->_setptr = mb_set;
	psg->_getptr = mb_get;
	psg->index = p_index;
	psg->type = p_pinfo.type;

	property_setget[p_pinfo.name] = psg;
	self_property_setget[p_pinfo.name] = psg;
}

void GDType::add_linked_property(const String &p_property, const String &p_linked_property) {
#ifdef TOOLS_ENABLED
	ERR_FAIL_COND(!Thread::is_main_thread());
	ERR_FAIL_COND(init_state != InitState::MUTABLE);
	ERR_FAIL_COND(!property_map.has(p_property));
	ERR_FAIL_COND(!property_map.has(p_linked_property));

	if (!linked_properties.has(p_property)) {
		linked_properties.insert(p_property, memnew(List<StringName>()));
		self_linked_properties.insert(p_property, memnew(List<StringName>()));
	}
	linked_properties[p_property]->push_back(p_linked_property);
	self_linked_properties[p_property]->push_back(p_linked_property);
#endif
}
