
/*
 * Field.cxx
 *
 *  Created on: Mar 18, 2010
 *      Author: jamitch
 */
#include "Field.h"

namespace Field_NS {


/*
 * Static variable that must be initialized
 */
const map<string, FieldSpec> FieldSpecMap::Map =  FieldSpecMap::create_map();

std::ostream& FieldSpec::print(std::ostream& os) const {
	os << label;
	return os;
}

/**
 * FieldSpec Implementation
 */

// Default constructor
FieldSpec::FieldSpec()
:
		type(FIELDSPEC_UNDEFINED.getType()),
		relation(FIELDSPEC_UNDEFINED.getRelation()),
		length(FIELDSPEC_UNDEFINED.getLength()),
		par_top(FIELDSPEC_UNDEFINED.get_parallel_topology()),
		temporal(FIELDSPEC_UNDEFINED.get_temporal()),
		id(FIELDSPEC_UNDEFINED.getId()),
		label(FIELDSPEC_UNDEFINED.getLabel())
{}

FieldSpec::
FieldSpec(Field_ENUM::Type t, Field_ENUM::Relation r,  Field_ENUM::Length len, Field_ENUM::Temporal temp, const string& label)
:
		type(t),    relation(r),        length(len),             par_top(Field_ENUM::OWNED), temporal(temp),
		id(type | (relation<<8) | (length << (8+4)) | (par_top << (8+4+4)) |            (temporal << (8+4+4+4))),
		label(label)
{}

FieldSpec::
FieldSpec(Field_ENUM::Type t, Field_ENUM::Relation r,  Field_ENUM::Length len, Field_ENUM::ParallelTopology p, Field_ENUM::Temporal temp, const string& label)
:
	type(t),    relation(r),        length(len),             par_top(p), temporal(temp),
	id(type | (relation<<8) | (length << (8+4)) | (par_top << (8+4+4)) |            (temporal << (8+4+4+4))),
	label(label)
{}

FieldSpec::FieldSpec(const FieldSpec& c)
:
		type(c.getType()),
		relation(c.getRelation()),
		length(c.getLength()),
		par_top(c.get_parallel_topology()),
		temporal(c.get_temporal()),
		id(c.getId()),
		label(c.getLabel()) {}

const FieldSpec& FieldSpec::operator=(const FieldSpec& right)  {
	if(this != &right){
		type = right.getType();
		relation=right.getRelation();
		length = right.getLength();
		par_top=right.get_parallel_topology();
		temporal = right.get_temporal();
		id=right.getId();
		label = right.getLabel();
	}
	return *this;
}

bool FieldSpec::operator == (const FieldSpec& right) const {
  return (id == right.getId()) ? true : false;
}

bool FieldSpec::operator != (const FieldSpec& right) const {
	return !(*this==right);
}

bool FieldSpec::operator < (const FieldSpec& right) const {
	return id < right.getId();
}

const FieldSpec FieldSpec::get_overlap_spec() const {
	return FieldSpec(type,relation,length,Field_ENUM::OVERLAP,temporal,label);
}

const FieldSpec FieldSpec::get_override(Field_ENUM::Temporal override) const {
	return FieldSpec(type,relation,length,par_top,override,label);
}

} // Field
