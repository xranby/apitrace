/**************************************************************************
 *
 * Copyright 2011 Jose Fonseca
 * Copyright 2010 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 **************************************************************************/


#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "trace_file.hpp"
#include "trace_parser.hpp"


#define TRACE_VERBOSE 0


namespace trace {


Parser::Parser() {
    file = NULL;
    next_call_no = 0;
    version = 0;

    glGetErrorSig = NULL;
}


Parser::~Parser() {
    close();
}


bool Parser::open(const char *filename) {
    assert(!file);
    file = File::createForRead(filename);
    if (!file) {
        return false;
    }

    version = read_uint();
    if (version > TRACE_VERSION) {
        std::cerr << "error: unsupported trace format version " << version << "\n";
        return false;
    }

    return true;
}

template <typename Iter>
inline void
deleteAll(Iter begin, Iter end)
{
    while (begin != end) {
        delete *begin;
        ++begin;
    }
}

template <typename Container>
inline void
deleteAll(Container &c)
{
    deleteAll(c.begin(), c.end());
    c.clear();
}

void Parser::close(void) {
    if (file) {
        file->close();
        delete file;
        file = NULL;
    }

    deleteAll(calls);

    // Delete all signature data.  Signatures are mere structures which don't
    // own their own memory, so we need to destroy all data we created here.

    for (FunctionMap::iterator it = functions.begin(); it != functions.end(); ++it) {
        FunctionSigState *sig = *it;
        if (sig) {
            delete [] sig->name;
            for (unsigned arg = 0; arg < sig->num_args; ++arg) {
                delete [] sig->arg_names[arg];
            }
            delete [] sig->arg_names;
            delete sig;
        }
    }
    functions.clear();

    for (StructMap::iterator it = structs.begin(); it != structs.end(); ++it) {
        StructSigState *sig = *it;
        if (sig) {
            delete [] sig->name;
            for (unsigned member = 0; member < sig->num_members; ++member) {
                delete [] sig->member_names[member];
            }
            delete [] sig->member_names;
            delete sig;
        }
    }
    structs.clear();

    for (EnumMap::iterator it = enums.begin(); it != enums.end(); ++it) {
        EnumSigState *sig = *it;
        if (sig) {
            for (unsigned value = 0; value < sig->num_values; ++value) {
                delete [] sig->values[value].name;
            }
            delete [] sig->values;
            delete sig;
        }
    }
    enums.clear();
    
    for (BitmaskMap::iterator it = bitmasks.begin(); it != bitmasks.end(); ++it) {
        BitmaskSigState *sig = *it;
        if (sig) {
            for (unsigned flag = 0; flag < sig->num_flags; ++flag) {
                delete [] sig->flags[flag].name;
            }
            delete [] sig->flags;
            delete sig;
        }
    }
    bitmasks.clear();

    next_call_no = 0;
}


void Parser::getBookmark(ParseBookmark &bookmark) {
    bookmark.offset = file->currentOffset();
    bookmark.next_call_no = next_call_no;
}


void Parser::setBookmark(const ParseBookmark &bookmark) {
    file->setCurrentOffset(bookmark.offset);
    next_call_no = bookmark.next_call_no;
    
    // Simply ignore all pending calls
    deleteAll(calls);
}


Call *Parser::parse_call(Mode mode) {
    do {
        Call *call;
        int c = read_byte();
        switch (c) {
        case trace::EVENT_ENTER:
            parse_enter(mode);
            break;
        case trace::EVENT_LEAVE:
            call = parse_leave(mode);
            adjust_call_flags(call);
            return call;
        default:
            std::cerr << "error: unknown event " << c << "\n";
            exit(1);
        case -1:
            if (!calls.empty()) {
                call = calls.front();
                call->flags |= CALL_FLAG_INCOMPLETE;
                calls.pop_front();
                adjust_call_flags(call);
                return call;
            }
            return NULL;
        }
    } while(true);
}


/**
 * Helper function to lookup an ID in a vector, resizing the vector if it doesn't fit.
 */
template<class T>
T *lookup(std::vector<T *> &map, size_t index) {
    if (index >= map.size()) {
        map.resize(index + 1);
        return NULL;
    } else {
        return map[index];
    }
}


Parser::FunctionSigFlags *
Parser::parse_function_sig(void) {
    size_t id = read_uint();

    FunctionSigState *sig = lookup(functions, id);

    if (!sig) {
        /* parse the signature */
        sig = new FunctionSigState;
        sig->id = id;
        sig->name = read_string();
        sig->num_args = read_uint();
        const char **arg_names = new const char *[sig->num_args];
        for (unsigned i = 0; i < sig->num_args; ++i) {
            arg_names[i] = read_string();
        }
        sig->arg_names = arg_names;
        sig->flags = lookupCallFlags(sig->name);
        sig->offset = file->currentOffset();
        functions[id] = sig;

        /**
         * Note down the signature of special functions for future reference.
         *
         * NOTE: If the number of comparisons increases we should move this to a
         * separate function and use bisection.
         */
        if (sig->num_args == 0 &&
            strcmp(sig->name, "glGetError") == 0) {
            glGetErrorSig = sig;
        }

    } else if (file->currentOffset() < sig->offset) {
        /* skip over the signature */
        skip_string(); /* name */
        unsigned num_args = read_uint();
        for (unsigned i = 0; i < num_args; ++i) {
             skip_string(); /*arg_name*/
        }
    }

    assert(sig);
    return sig;
}


StructSig *Parser::parse_struct_sig() {
    size_t id = read_uint();

    StructSigState *sig = lookup(structs, id);

    if (!sig) {
        /* parse the signature */
        sig = new StructSigState;
        sig->id = id;
        sig->name = read_string();
        sig->num_members = read_uint();
        const char **member_names = new const char *[sig->num_members];
        for (unsigned i = 0; i < sig->num_members; ++i) {
            member_names[i] = read_string();
        }
        sig->member_names = member_names;
        sig->offset = file->currentOffset();
        structs[id] = sig;
    } else if (file->currentOffset() < sig->offset) {
        /* skip over the signature */
        skip_string(); /* name */
        unsigned num_members = read_uint();
        for (unsigned i = 0; i < num_members; ++i) {
            skip_string(); /* member_name */
        }
    }

    assert(sig);
    return sig;
}


/*
 * Old enum signatures would cover a single name/value only:
 *
 *   enum_sig = id name value
 *            | id
 */
EnumSig *Parser::parse_old_enum_sig() {
    size_t id = read_uint();

    EnumSigState *sig = lookup(enums, id);

    if (!sig) {
        /* parse the signature */
        sig = new EnumSigState;
        sig->id = id;
        sig->num_values = 1;
        EnumValue *values = new EnumValue[sig->num_values];
        values->name = read_string();
        values->value = read_sint();
        sig->values = values;
        sig->offset = file->currentOffset();
        enums[id] = sig;
    } else if (file->currentOffset() < sig->offset) {
        /* skip over the signature */
        skip_string(); /*name*/
        scan_value();
    }

    assert(sig);
    return sig;
}


EnumSig *Parser::parse_enum_sig() {
    size_t id = read_uint();

    EnumSigState *sig = lookup(enums, id);

    if (!sig) {
        /* parse the signature */
        sig = new EnumSigState;
        sig->id = id;
        sig->num_values = read_uint();
        EnumValue *values = new EnumValue[sig->num_values];
        for (EnumValue *it = values; it != values + sig->num_values; ++it) {
            it->name = read_string();
            it->value = read_sint();
        }
        sig->values = values;
        sig->offset = file->currentOffset();
        enums[id] = sig;
    } else if (file->currentOffset() < sig->offset) {
        /* skip over the signature */
        int num_values = read_uint();
        for (int i = 0; i < num_values; ++i) {
            skip_string(); /*name */
            skip_sint(); /* value */
        }
    }

    assert(sig);
    return sig;
}


BitmaskSig *Parser::parse_bitmask_sig() {
    size_t id = read_uint();

    BitmaskSigState *sig = lookup(bitmasks, id);

    if (!sig) {
        /* parse the signature */
        sig = new BitmaskSigState;
        sig->id = id;
        sig->num_flags = read_uint();
        BitmaskFlag *flags = new BitmaskFlag[sig->num_flags];
        for (BitmaskFlag *it = flags; it != flags + sig->num_flags; ++it) {
            it->name = read_string();
            it->value = read_uint();
            if (it->value == 0 && it != flags) {
                std::cerr << "warning: bitmask " << it->name << " is zero but is not first flag\n";
            }
        }
        sig->flags = flags;
        sig->offset = file->currentOffset();
        bitmasks[id] = sig;
    } else if (file->currentOffset() < sig->offset) {
        /* skip over the signature */
        int num_flags = read_uint();
        for (int i = 0; i < num_flags; ++i) {
            skip_string(); /*name */
            skip_uint(); /* value */
        }
    }

    assert(sig);
    return sig;
}


void Parser::parse_enter(Mode mode) {
    unsigned thread_id;

    if (version >= 4) {
        thread_id = read_uint();
    } else {
        thread_id = 0;
    }

    FunctionSigFlags *sig = parse_function_sig();

    Call *call = new Call(sig, sig->flags, thread_id);

    call->no = next_call_no++;

    if (parse_call_details(call, mode)) {
        calls.push_back(call);
    } else {
        delete call;
    }
}


Call *Parser::parse_leave(Mode mode) {
    Value* call_time = parse_uint();
    unsigned call_no = read_uint();
    Call *call = NULL;
    for (CallList::iterator it = calls.begin(); it != calls.end(); ++it) {
        if ((*it)->no == call_no) {
            call = *it;
            calls.erase(it);
            break;
        }
    }
    if (!call) {
        return NULL;
    }

    call->call_time = call_time;
    if (parse_call_details(call, mode)) {
        return call;
    } else {
        delete call;
        return NULL;
    }
}


bool Parser::parse_call_details(Call *call, Mode mode) {
    do {
        int c = read_byte();
        switch (c) {
        case trace::CALL_END:
            return true;
        case trace::CALL_ARG:
            parse_arg(call, mode);
            break;
        case trace::CALL_RET:
            call->ret = parse_value(mode);
            break;
        default:
            std::cerr << "error: ("<<call->name()<< ") unknown call detail "
                      << c << "\n";
            exit(1);
        case -1:
            return false;
        }
    } while(true);
}


/**
 * Make adjustments to this particular call flags.
 *
 * NOTE: This is called per-call so no string comparisons should be done here.
 * All name comparisons should be done when the signature is parsed instead.
 */
void Parser::adjust_call_flags(Call *call) {
    // Mark glGetError() = GL_NO_ERROR as verbose
    if (call->sig == glGetErrorSig &&
        call->ret &&
        call->ret->toSInt() == 0) {
        call->flags |= CALL_FLAG_VERBOSE;
    }
}

void Parser::parse_arg(Call *call, Mode mode) {
    unsigned index = read_uint();
    Value *value = parse_value(mode);
    if (value) {
        if (index >= call->args.size()) {
            call->args.resize(index + 1);
        }
        call->args[index] = value;
    }
}


Value *Parser::parse_value(void) {
    int c;
    Value *value;
    c = read_byte();
    switch (c) {
    case trace::TYPE_NULL:
        value = new Null;
        break;
    case trace::TYPE_FALSE:
        value = new Bool(false);
        break;
    case trace::TYPE_TRUE:
        value = new Bool(true);
        break;
    case trace::TYPE_SINT:
        value = parse_sint();
        break;
    case trace::TYPE_UINT:
        value = parse_uint();
        break;
    case trace::TYPE_FLOAT:
        value = parse_float();
        break;
    case trace::TYPE_DOUBLE:
        value = parse_double();
        break;
    case trace::TYPE_STRING:
        value = parse_string();
        break;
    case trace::TYPE_ENUM:
        value = parse_enum();
        break;
    case trace::TYPE_BITMASK:
        value = parse_bitmask();
        break;
    case trace::TYPE_ARRAY:
        value = parse_array();
        break;
    case trace::TYPE_STRUCT:
        value = parse_struct();
        break;
    case trace::TYPE_BLOB:
        value = parse_blob();
        break;
    case trace::TYPE_OPAQUE:
        value = parse_opaque();
        break;
    default:
        std::cerr << "error: unknown type " << c << "\n";
        exit(1);
    case -1:
        value = NULL;
        break;
    }
#if TRACE_VERBOSE
    if (value) {
        std::cerr << "\tVALUE " << value << "\n";
    }
#endif
    return value;
}


void Parser::scan_value(void) {
    int c = read_byte();
    switch (c) {
    case trace::TYPE_NULL:
    case trace::TYPE_FALSE:
    case trace::TYPE_TRUE:
        break;
    case trace::TYPE_SINT:
        scan_sint();
        break;
    case trace::TYPE_UINT:
        scan_uint();
        break;
    case trace::TYPE_FLOAT:
        scan_float();
        break;
    case trace::TYPE_DOUBLE:
        scan_double();
        break;
    case trace::TYPE_STRING:
        scan_string();
        break;
    case trace::TYPE_ENUM:
        scan_enum();
        break;
    case trace::TYPE_BITMASK:
        scan_bitmask();
        break;
    case trace::TYPE_ARRAY:
        scan_array();
        break;
    case trace::TYPE_STRUCT:
        scan_struct();
        break;
    case trace::TYPE_BLOB:
        scan_blob();
        break;
    case trace::TYPE_OPAQUE:
        scan_opaque();
        break;
    default:
        std::cerr << "error: unknown type " << c << "\n";
        exit(1);
    case -1:
        break;
    }
}


Value *Parser::parse_sint() {
    return new SInt(-(signed long long)read_uint());
}


void Parser::scan_sint() {
    skip_uint();
}


Value *Parser::parse_uint() {
    return new UInt(read_uint());
}


void Parser::scan_uint() {
    skip_uint();
}


Value *Parser::parse_float() {
    float value;
    file->read(&value, sizeof value);
    return new Float(value);
}


void Parser::scan_float() {
    file->skip(sizeof(float));
}


Value *Parser::parse_double() {
    double value;
    file->read(&value, sizeof value);
    return new Double(value);
}


void Parser::scan_double() {
    file->skip(sizeof(double));
}


Value *Parser::parse_string() {
    return new String(read_string());
}


void Parser::scan_string() {
    skip_string();
}


Value *Parser::parse_enum() {
    EnumSig *sig;
    signed long long value;
    if (version >= 3) {
        sig = parse_enum_sig();
        value = read_sint();
    } else {
        sig = parse_old_enum_sig();
        assert(sig->num_values == 1);
        value = sig->values->value;
    }
    return new Enum(sig, value);
}


void Parser::scan_enum() {
    if (version >= 3) {
        parse_enum_sig();
        skip_sint();
    } else {
        parse_old_enum_sig();
    }
}


Value *Parser::parse_bitmask() {
    BitmaskSig *sig = parse_bitmask_sig();

    unsigned long long value = read_uint();

    return new Bitmask(sig, value);
}


void Parser::scan_bitmask() {
    parse_bitmask_sig();
    skip_uint(); /* value */
}


Value *Parser::parse_array(void) {
    size_t len = read_uint();
    Array *array = new Array(len);
    for (size_t i = 0; i < len; ++i) {
        array->values[i] = parse_value();
    }
    return array;
}


void Parser::scan_array(void) {
    size_t len = read_uint();
    for (size_t i = 0; i < len; ++i) {
        scan_value();
    }
}


Value *Parser::parse_blob(void) {
    size_t size = read_uint();
    Blob *blob = new Blob(size);
    if (size) {
        file->read(blob->buf, (unsigned)size);
    }
    return blob;
}


void Parser::scan_blob(void) {
    size_t size = read_uint();
    if (size) {
        file->skip(size);
    }
}


Value *Parser::parse_struct() {
    StructSig *sig = parse_struct_sig();
    Struct *value = new Struct(sig);

    for (size_t i = 0; i < sig->num_members; ++i) {
        value->members[i] = parse_value();
    }

    return value;
}


void Parser::scan_struct() {
    StructSig *sig = parse_struct_sig();
    for (size_t i = 0; i < sig->num_members; ++i) {
        scan_value();
    }
}


Value *Parser::parse_opaque() {
    unsigned long long addr;
    addr = read_uint();
    return new Pointer(addr);
}


void Parser::scan_opaque() {
    skip_uint();
}


const char * Parser::read_string(void) {
    size_t len = read_uint();
    char * value = new char[len + 1];
    if (len) {
        file->read(value, (unsigned)len);
    }
    value[len] = 0;
#if TRACE_VERBOSE
    std::cerr << "\tSTRING \"" << value << "\"\n";
#endif
    return value;
}


void Parser::skip_string(void) {
    size_t len = read_uint();
    file->skip(len);
}


/*
 * For the time being, a signed int is encoded as any other value, but we here parse
 * it without the extra baggage of the Value class.
 */
signed long long
Parser::read_sint(void) {
    int c;
    c = read_byte();
    switch (c) {
    case trace::TYPE_SINT:
        return -read_uint();
    case trace::TYPE_UINT:
        return read_uint();
    default:
        std::cerr << "error: unexpected type " << c << "\n";
        exit(1);
    case -1:
        return 0;
    }
}

void
Parser::skip_sint(void) {
    skip_byte();
    skip_uint();
}

unsigned long long Parser::read_uint(void) {
    unsigned long long value = 0;
    int c;
    unsigned shift = 0;
    do {
        c = file->getc();
        if (c == -1) {
            break;
        }
        value |= (unsigned long long)(c & 0x7f) << shift;
        shift += 7;
    } while(c & 0x80);
#if TRACE_VERBOSE
    std::cerr << "\tUINT " << value << "\n";
#endif
    return value;
}


void Parser::skip_uint(void) {
    int c;
    do {
        c = file->getc();
        if (c == -1) {
            break;
        }
    } while(c & 0x80);
}


inline int Parser::read_byte(void) {
    int c = file->getc();
#if TRACE_VERBOSE
    if (c < 0)
        std::cerr << "\tEOF" << "\n";
    else
        std::cerr << "\tBYTE 0x" << std::hex << c << std::dec << "\n";
#endif
    return c;
}


inline void Parser::skip_byte(void) {
    file->skip(1);
}


} /* namespace trace */
