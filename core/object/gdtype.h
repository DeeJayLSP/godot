/**************************************************************************/
/*  gdtype.h                                                              */
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

#pragma once

#include "core/object/method_info.h"
#include "core/string/string_name.h"
#include "core/templates/a_hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/vector.h"

class MethodBind;

class GDType {
public:
	enum class InitState {
		UNINITIALIZED,
		MUTABLE,
		FINALIZED,
	};

	struct EnumInfo {
		StringName name;
		AHashMap<StringName, int64_t> values;
		bool is_bitfield = false;
	};

	struct PropertySetGet {
		int index;
		StringName setter;
		StringName getter;
		const MethodBind *_setptr = nullptr;
		const MethodBind *_getptr = nullptr;
		Variant::Type type;
	};

protected:
	const GDType *super_type;
	mutable InitState init_state = InitState::UNINITIALIZED;

	StringName name;
	/// Contains all the class names in order:
	/// `name` is the first element and `Object` is the last.
	Vector<StringName> name_hierarchy;

	AHashMap<StringName, int64_t> constant_map;
	AHashMap<StringName, int64_t> self_constant_map;

	AHashMap<StringName, const EnumInfo *> enum_map;
	AHashMap<StringName, const EnumInfo *> self_enum_map;

	AHashMap<StringName, const MethodInfo *> signal_map;
	AHashMap<StringName, const MethodInfo *> self_signal_map;

	AHashMap<StringName, const MethodBind *> method_map;
	AHashMap<StringName, const MethodBind *> self_method_map;

	List<const PropertyInfo *> property_list;
	List<const PropertyInfo *> self_property_list;

	AHashMap<StringName, const PropertyInfo *> property_map;
	AHashMap<StringName, const PropertyInfo *> self_property_map;

	AHashMap<StringName, const PropertySetGet *> property_setget;
	AHashMap<StringName, const PropertySetGet *> self_property_setget;

#ifdef DEBUG_ENABLED
	HashSet<StringName> methods_in_properties;
	HashSet<StringName> self_methods_in_properties;
#endif

#ifdef TOOLS_ENABLED
	HashMap<StringName, List<StringName> *> linked_properties;
	HashMap<StringName, List<StringName> *> self_linked_properties;
#endif

	// Lifecycle
	friend class ClassDB;
	void free_method_binds();

public:
	GDType(const GDType *p_super_type, StringName p_name);
	~GDType();

	InitState get_init_state() const { return init_state; }
	void initialize();

	const GDType *get_super_type() const { return super_type; }
	const StringName &get_name() const { return name; }
	const StringName &get_super_type_name() const {
		static const StringName EMPTY;
		return super_type ? super_type->name : EMPTY;
	}
	const Vector<StringName> &get_name_hierarchy() const { return name_hierarchy; }

	void bind_integer_constant(const StringName &p_enum, const StringName &p_name, int64_t p_constant, bool p_is_bitfield = false);
	const AHashMap<StringName, int64_t> &get_integer_constant_map(bool p_no_inheritance = false) const { return p_no_inheritance ? self_constant_map : constant_map; }
	const AHashMap<StringName, const EnumInfo *> &get_enum_map(bool p_no_inheritance = false) const { return p_no_inheritance ? self_enum_map : enum_map; }
	const EnumInfo *get_integer_constant_enum(const StringName &p_name, bool p_no_inheritance = false) const;

	void add_signal(MethodInfo p_signal);
	const AHashMap<StringName, const MethodInfo *> &get_signal_map(bool p_no_inheritance = false) const { return p_no_inheritance ? self_signal_map : signal_map; }

	bool bind_method(MethodBind *p_method);
	void set_method_flags(const StringName &p_method, int p_flags);
	const AHashMap<StringName, const MethodBind *> &get_method_map(bool p_no_inheritance = false) const { return p_no_inheritance ? self_method_map : method_map; }

	void add_property_group(const String &p_name, const String &p_prefix = "", int p_indent_depth = 0);
	void add_property_subgroup(const String &p_name, const String &p_prefix = "", int p_indent_depth = 0);
	void add_property_array_count(const String &p_label, const StringName &p_count_property, const StringName &p_count_setter, const StringName &p_count_getter, const String &p_array_element_prefix, uint32_t p_count_usage = PROPERTY_USAGE_DEFAULT);
	void add_property_array(const StringName &p_path, const String &p_array_element_prefix);
	void add_property(const PropertyInfo &p_info, const StringName &p_setter, const StringName &p_getter, int p_index = -1);
	void add_linked_property(const String &p_property, const String &p_linked_property);
	const List<const PropertyInfo *> &get_property_list(bool p_no_inheritance = false) const { return p_no_inheritance ? self_property_list : property_list; }
	const AHashMap<StringName, const PropertyInfo *> &get_property_map(bool p_no_inheritance = false) const { return p_no_inheritance ? self_property_map : property_map; }
	const AHashMap<StringName, const PropertySetGet *> &get_property_setget(bool p_no_inheritance = false) const { return p_no_inheritance ? self_property_setget : property_setget; }
#ifdef DEBUG_ENABLED
	const HashSet<StringName> &get_methods_in_properties(bool p_no_inheritance = false) const { return p_no_inheritance ? self_methods_in_properties : methods_in_properties; }
#endif
#ifdef TOOLS_ENABLED
	const HashMap<StringName, List<StringName> *> &get_linked_properties(bool p_no_inheritance = false) const { return p_no_inheritance ? self_linked_properties : linked_properties; }
#endif
};
