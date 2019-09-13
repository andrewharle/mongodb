"""GDB Pretty-printers for MongoDB."""
from __future__ import print_function

import struct
import sys

import gdb.printing

try:
    import bson
    import bson.json_util
    import collections
    from bson.codec_options import CodecOptions
except ImportError as err:
    print("Warning: Could not load bson library for Python '" + str(sys.version) + "'.")
    print("Check with the pip command if pymongo 3.x is installed.")
    bson = None


def get_unique_ptr(obj):
    """Read the value of a libstdc++ std::unique_ptr."""
    return obj["_M_t"]['_M_head_impl']


###################################################################################################
#
# Pretty-Printers
#
###################################################################################################


class StatusPrinter(object):
    """Pretty-printer for mongo::Status."""

    def __init__(self, val):
        """Initialize StatusPrinter."""
        self.val = val

    def to_string(self):
        """Return status for printing."""
        if not self.val['_error']:
            return 'Status::OK()'

        code = self.val['_error']['code']
        # Remove the mongo::ErrorCodes:: prefix. Does nothing if not a real ErrorCode.
        code = str(code).split('::')[-1]

        info = self.val['_error'].dereference()
        reason = info['reason']
        return 'Status(%s, %s)' % (code, reason)


class StatusWithPrinter(object):
    """Pretty-printer for mongo::StatusWith<>."""

    def __init__(self, val):
        """Initialize StatusWithPrinter."""
        self.val = val

    def to_string(self):
        """Return status for printing."""
        if not self.val['_status']['_error']:
            return 'StatusWith(OK, %s)' % (self.val['_t'])

        code = self.val['_status']['_error']['code']

        # Remove the mongo::ErrorCodes:: prefix. Does nothing if not a real ErrorCode.
        code = str(code).split('::')[-1]

        info = self.val['_status']['_error'].dereference()
        reason = info['reason']
        return 'StatusWith(%s, %s)' % (code, reason)


class StringDataPrinter(object):
    """Pretty-printer for mongo::StringData."""

    def __init__(self, val):
        """Initialize StringDataPrinter."""
        self.val = val

    @staticmethod
    def display_hint():
        """Display hint."""
        return 'string'

    def to_string(self):
        """Return data for printing."""
        size = self.val["_size"]
        if size == -1:
            return self.val['_data'].lazy_string()
        return self.val['_data'].lazy_string(length=size)


class BSONObjPrinter(object):
    """Pretty-printer for mongo::BSONObj."""

    def __init__(self, val):
        """Initialize BSONObjPrinter."""
        self.val = val
        self.ptr = self.val['_objdata'].cast(gdb.lookup_type('void').pointer())
        # Handle the endianness of the BSON object size, which is represented as a 32-bit integer
        # in little-endian format.
        inferior = gdb.selected_inferior()
        if self.ptr.is_optimized_out:
            # If the value has been optimized out, we cannot decode it.
            self.size = -1
        else:
            self.size = struct.unpack('<I', inferior.read_memory(self.ptr, 4))[0]

    @staticmethod
    def display_hint():
        """Display hint."""
        return 'map'

    def children(self):
        """Children."""
        # Do not decode a BSONObj with an invalid size.
        if not bson or self.size < 5 or self.size > 17 * 1024 * 1024:
            return

        inferior = gdb.selected_inferior()
        buf = bytes(inferior.read_memory(self.ptr, self.size))
        options = CodecOptions(document_class=collections.OrderedDict)
        bsondoc = bson.BSON.decode(buf, codec_options=options)

        for key, val in bsondoc.items():
            yield 'key', key
            yield 'value', bson.json_util.dumps(val)

    def to_string(self):
        """Return BSONObj for printing."""
        # The value has been optimized out.
        if self.size == -1:
            return "BSONObj @ %s" % (self.ptr)

        ownership = "owned" if self.val['_ownedBuffer']['_buffer']['_holder']['px'] else "unowned"

        size = self.size
        # Print an invalid BSONObj size in hex.
        if size < 5 or size > 17 * 1024 * 1024:
            size = hex(size)

        if size == 5:
            return "%s empty BSONObj @ %s" % (ownership, self.ptr)
        return "%s BSONObj %s bytes @ %s" % (ownership, size, self.ptr)


class UnorderedFastKeyTablePrinter(object):
    """Pretty-printer for mongo::UnorderedFastKeyTable<>."""

    def __init__(self, val):
        """Initialize UnorderedFastKeyTablePrinter."""
        self.val = val

        # Get the value_type by doing a type lookup
        value_type_name = val.type.strip_typedefs().name + "::value_type"
        value_type = gdb.lookup_type(value_type_name).target()
        self.value_type_ptr = value_type.pointer()

    @staticmethod
    def display_hint():
        """Display hint."""
        return 'map'

    def to_string(self):
        """Return UnorderedFastKeyTablePrinter for printing."""
        return "UnorderedFastKeyTablePrinter<%s> with %s elems " % (
            self.val.type.template_argument(0), self.val["_size"])

    def children(self):
        """Children."""
        cap = self.val["_area"]["_hashMask"] + 1
        it = get_unique_ptr(self.val["_area"]["_entries"])
        end = it + cap

        if it == 0:
            return

        while it != end:
            elt = it.dereference()
            it += 1
            if not elt['_used']:
                continue

            value = elt['_data']["__data"].cast(self.value_type_ptr).dereference()

            yield ('key', value['first'])
            yield ('value', value['second'])


class DecorablePrinter(object):
    """Pretty-printer for mongo::Decorable<>."""

    def __init__(self, val):
        """Initialize DecorablePrinter."""
        self.val = val

        decl_vector = val["_decorations"]["_registry"]["_decorationInfo"]
        # TODO: abstract out navigating a std::vector
        self.start = decl_vector["_M_impl"]["_M_start"]
        finish = decl_vector["_M_impl"]["_M_finish"]
        decorable_t = val.type.template_argument(0)
        decinfo_t = gdb.lookup_type(
            'mongo::DecorationRegistry<{}>::DecorationInfo'.format(decorable_t))
        self.count = int((int(finish) - int(self.start)) / decinfo_t.sizeof)

    @staticmethod
    def display_hint():
        """Display hint."""
        return 'map'

    def to_string(self):
        """Return Decorable for printing."""
        return "Decorable<%s> with %s elems " % (self.val.type.template_argument(0), self.count)

    def children(self):
        """Children."""
        decoration_data = get_unique_ptr(self.val["_decorations"]["_decorationData"])

        for index in range(self.count):
            descriptor = self.start[index]
            dindex = int(descriptor["descriptor"]["_index"])

            # In order to get the type stored in the decorable, we examine the type of its
            # constructor, and do some string manipulations.
            # TODO: abstract out navigating a std::function
            type_name = str(descriptor["constructor"])
            type_name = type_name[0:len(type_name) - 1]
            type_name = type_name[0:type_name.rindex(">")]
            type_name = type_name[type_name.index("constructAt<"):].replace("constructAt<", "")

            # If the type is a pointer type, strip the * at the end.
            if type_name.endswith('*'):
                type_name = type_name[0:len(type_name) - 1]
            type_name = type_name.rstrip()

            # Cast the raw char[] into the actual object that is stored there.
            type_t = gdb.lookup_type(type_name)
            obj = decoration_data[dindex].cast(type_t)

            yield ('key', "%d:%s:%s" % (index, obj.address, type_name))
            yield ('value', obj)


def find_match_brackets(search, opening='<', closing='>'):
    """Return the index of the closing bracket that matches the first opening bracket.

    Return -1 if no last matching bracket is found, i.e. not a template.

    Example:
        'Foo<T>::iterator<U>''
        returns 5
    """
    index = search.find(opening)
    if index == -1:
        return -1

    start = index + 1
    count = 1
    str_len = len(search)
    for index in range(start, str_len):
        char = search[index]

        if char == opening:
            count += 1
        elif char == closing:
            count -= 1

        if count == 0:
            return index

    return -1


class MongoSubPrettyPrinter(gdb.printing.SubPrettyPrinter):
    """Sub pretty printer managed by the pretty-printer collection."""

    def __init__(self, name, prefix, is_template, printer):
        """Initialize MongoSubPrettyPrinter."""
        super(MongoSubPrettyPrinter, self).__init__(name)
        self.prefix = prefix
        self.printer = printer
        self.is_template = is_template


class MongoPrettyPrinterCollection(gdb.printing.PrettyPrinter):
    """MongoDB-specific printer printer collection that ignores subtypes.

    It will match 'HashTable<T> but not 'HashTable<T>::iterator' when asked for 'HashTable'.
    """

    def __init__(self):
        """Initialize MongoPrettyPrinterCollection."""
        super(MongoPrettyPrinterCollection, self).__init__("mongo", [])

    def add(self, name, prefix, is_template, printer):
        """Add a subprinter."""
        self.subprinters.append(MongoSubPrettyPrinter(name, prefix, is_template, printer))

    def __call__(self, val):
        """Return matched printer type."""

        # Get the type name.
        lookup_tag = gdb.types.get_basic_type(val.type).tag
        if not lookup_tag:
            lookup_tag = val.type.name
        if not lookup_tag:
            return None

        index = find_match_brackets(lookup_tag)

        # Ignore subtypes of classes
        # We do not want HashTable<T>::iterator as an example, just HashTable<T>
        if index == -1 or index + 1 == len(lookup_tag):
            for printer in self.subprinters:
                if not printer.enabled:
                    continue
                if ((not printer.is_template or lookup_tag.find(printer.prefix) != 0)
                        and (printer.is_template or lookup_tag != printer.prefix)):
                    continue
                return printer.printer(val)

        return None


def build_pretty_printer():
    """Build a pretty printer."""
    pp = MongoPrettyPrinterCollection()
    pp.add('BSONObj', 'mongo::BSONObj', False, BSONObjPrinter)
    pp.add('Decorable', 'mongo::Decorable', True, DecorablePrinter)
    pp.add('Status', 'mongo::Status', False, StatusPrinter)
    pp.add('StatusWith', 'mongo::StatusWith', True, StatusWithPrinter)
    pp.add('StringData', 'mongo::StringData', False, StringDataPrinter)
    pp.add('UnorderedFastKeyTable', 'mongo::UnorderedFastKeyTable', True,
           UnorderedFastKeyTablePrinter)
    return pp


###################################################################################################
#
# Setup
#
###################################################################################################

# Register pretty-printers, replace existing mongo printers
gdb.printing.register_pretty_printer(gdb.current_objfile(), build_pretty_printer(), True)

print("MongoDB GDB pretty-printers loaded")
