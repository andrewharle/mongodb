#!/usr/bin/env python2
#
# Copyright (C) 2018-present MongoDB, Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the Server Side Public License, version 1,
# as published by MongoDB, Inc.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# Server Side Public License for more details.
#
# You should have received a copy of the Server Side Public License
# along with this program. If not, see
# <http://www.mongodb.com/licensing/server-side-public-license>.
#
# As a special exception, the copyright holders give permission to link the
# code of portions of this program with the OpenSSL library under certain
# conditions as described in each individual source file and distribute
# linked combinations including the program with the OpenSSL library. You
# must comply with the Server Side Public License in all respects for
# all of the code used other than as permitted herein. If you modify file(s)
# with this exception, you may extend this exception to your version of the
# file(s), but you are not obligated to do so. If you do not wish to do so,
# delete this exception statement from your version. If you delete this
# exception statement from all source files in the program, then also delete
# it in the license file.
#
# pylint: disable=too-many-lines
"""Test cases for IDL binder."""

from __future__ import absolute_import, print_function, unicode_literals

import textwrap
import unittest

# import package so that it works regardless of whether we run as a module or file
if __package__ is None:
    import sys
    from os import path
    sys.path.append(path.dirname(path.abspath(__file__)))
    from context import idl
    import testcase
else:
    from .context import idl
    from . import testcase

# All YAML tests assume 4 space indent
INDENT_SPACE_COUNT = 4


def fill_spaces(count):
    # type: (int) -> unicode
    """Fill a string full of spaces."""
    fill = ''
    for _ in range(count * INDENT_SPACE_COUNT):
        fill += ' '

    return fill


def indent_text(count, unindented_text):
    # type: (int, unicode) -> unicode
    """Indent each line of a multi-line string."""
    lines = unindented_text.splitlines()
    fill = fill_spaces(count)
    return '\n'.join(fill + line for line in lines)


class TestBinder(testcase.IDLTestcase):
    """Test cases for the IDL binder."""

    # pylint: disable=too-many-public-methods

    def test_empty(self):
        # type: () -> None
        """Test an empty document works."""
        self.assert_bind("")

    def test_global_positive(self):
        # type: () -> None
        """Postive global tests."""
        spec = self.assert_bind(
            textwrap.dedent("""
        global:
            cpp_namespace: 'something'
            cpp_includes:
                - 'bar'
                - 'foo'"""))
        self.assertEquals(spec.globals.cpp_namespace, "something")
        self.assertListEqual(spec.globals.cpp_includes, ['bar', 'foo'])

    def test_type_positive(self):
        # type: () -> None
        """Positive type tests."""
        self.assert_bind(
            textwrap.dedent("""
        types:
            foo:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
                serializer: foo
                deserializer: foo
                default: foo
            """))

        # Test supported types
        for bson_type in [
                "bool", "date", "null", "decimal", "double", "int", "long", "objectid", "regex",
                "string", "timestamp", "undefined"
        ]:
            self.assert_bind(
                textwrap.dedent("""
        types:
            foofoo:
                description: foo
                cpp_type: foo
                bson_serialization_type: %s
                default: foo
                deserializer: BSONElement::fake
            """ % (bson_type)))

        # Test supported numeric types
        for cpp_type in [
                "std::int32_t",
                "std::uint32_t",
                "std::int32_t",
                "std::uint64_t",
                "std::vector<std::uint8_t>",
                "std::array<std::uint8_t, 16>",
        ]:
            self.assert_bind(
                textwrap.dedent("""
                types:
                    foofoo:
                        description: foo
                        cpp_type: %s
                        bson_serialization_type: int
                        deserializer: BSONElement::fake
                """ % (cpp_type)))

        # Test object
        self.assert_bind(
            textwrap.dedent("""
        types:
            foofoo:
                description: foo
                cpp_type: foo
                bson_serialization_type: object
                serializer: foo
                deserializer: foo
                default: foo
            """))

        # Test 'any'
        self.assert_bind(
            textwrap.dedent("""
        types:
            foofoo:
                description: foo
                cpp_type: foo
                bson_serialization_type: any
                serializer: foo
                deserializer: foo
                default: foo
            """))

        # Test 'chain'
        self.assert_bind(
            textwrap.dedent("""
        types:
            foofoo:
                description: foo
                cpp_type: foo
                bson_serialization_type: chain
                serializer: foo
                deserializer: foo
                default: foo
            """))

        # Test supported bindata_subtype
        for bindata_subtype in ["generic", "function", "uuid", "md5"]:
            self.assert_bind(
                textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: bindata
                    bindata_subtype: %s
                    deserializer: BSONElement::fake
                """ % (bindata_subtype)))

    def test_type_negative(self):
        # type: () -> None
        """Negative type tests for properties that types and fields share."""

        # Test bad bson type name
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: foo
            """), idl.errors.ERROR_ID_BAD_BSON_TYPE)

        # Test bad cpp_type name
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: StringData
                    bson_serialization_type: string
                    deserializer: bar
            """), idl.errors.ERROR_ID_NO_STRINGDATA)

        # Test unsupported serialization
        for cpp_type in [
                "char",
                "signed char",
                "unsigned char",
                "signed short int",
                "short int",
                "short",
                "signed short",
                "unsigned short",
                "unsigned short int",
                "signed int",
                "signed",
                "unsigned int",
                "unsigned",
                "signed long int",
                "signed long",
                "int",
                "long int",
                "long",
                "unsigned long int",
                "unsigned long",
                "signed long long int",
                "signed long long",
                "long long int",
                "long long",
                "unsigned long int",
                "unsigned long",
                "wchar_t",
                "char16_t",
                "char32_t",
                "int8_t",
                "int16_t",
                "int32_t",
                "int64_t",
                "uint8_t",
                "uint16_t",
                "uint32_t",
                "uint64_t",
        ]:
            self.assert_bind_fail(
                textwrap.dedent("""
                types:
                    foofoo:
                        description: foo
                        cpp_type: %s
                        bson_serialization_type: int
                        deserializer: BSONElement::int
                    """ % (cpp_type)), idl.errors.ERROR_ID_BAD_NUMERIC_CPP_TYPE)

        # Test the std prefix 8 and 16-byte integers fail
        for std_cpp_type in ["std::int8_t", "std::int16_t", "std::uint8_t", "std::uint16_t"]:
            self.assert_bind_fail(
                textwrap.dedent("""
                types:
                    foofoo:
                        description: foo
                        cpp_type: %s
                        bson_serialization_type: int
                        deserializer: BSONElement::int
                    """ % (std_cpp_type)), idl.errors.ERROR_ID_BAD_NUMERIC_CPP_TYPE)

        # Test bindata_subtype missing
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: bindata
                    deserializer: BSONElement::fake
            """), idl.errors.ERROR_ID_BAD_BSON_BINDATA_SUBTYPE_VALUE)

        # Test fake bindata_subtype is wrong
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: bindata
                    bindata_subtype: foo
                    deserializer: BSONElement::fake
            """), idl.errors.ERROR_ID_BAD_BSON_BINDATA_SUBTYPE_VALUE)

        # Test deprecated bindata_subtype 'binary', and 'uuid_old' are wrong
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: bindata
                    bindata_subtype: binary
            """), idl.errors.ERROR_ID_BAD_BSON_BINDATA_SUBTYPE_VALUE)

        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: bindata
                    bindata_subtype: uuid_old
            """), idl.errors.ERROR_ID_BAD_BSON_BINDATA_SUBTYPE_VALUE)

        # Test bindata_subtype on wrong type
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: string
                    bindata_subtype: generic
                    deserializer: BSONElement::fake
            """), idl.errors.ERROR_ID_BAD_BSON_BINDATA_SUBTYPE_TYPE)

        # Test bindata with default
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: bindata
                    bindata_subtype: uuid
                    default: 42
            """), idl.errors.ERROR_ID_BAD_BINDATA_DEFAULT)

        # Test bindata in list of types
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type:
                                - bindata
                                - string
            """), idl.errors.ERROR_ID_BAD_BSON_TYPE)

        # Test bindata in list of types
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: StringData
                    bson_serialization_type:
                                - bindata
                                - string
            """), idl.errors.ERROR_ID_BAD_BSON_TYPE)

        # Test 'any' in list of types
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type:
                                - any
                                - int
            """), idl.errors.ERROR_ID_BAD_ANY_TYPE_USE)

        # Test object in list of types
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type:
                                - object
                                - int
            """), idl.errors.ERROR_ID_BAD_BSON_TYPE_LIST)

        # Test fake in list of types
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type:
                                - int
                                - fake
            """), idl.errors.ERROR_ID_BAD_BSON_TYPE)

        # Test 'chain' in list of types
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type:
                                - chain
                                - int
            """), idl.errors.ERROR_ID_BAD_ANY_TYPE_USE)

        # Test unsupported serialization
        for bson_type in [
                "bool", "date", "null", "decimal", "double", "int", "long", "objectid", "regex",
                "timestamp", "undefined"
        ]:
            self.assert_bind_fail(
                textwrap.dedent("""
                types:
                    foofoo:
                        description: foo
                        cpp_type: std::string
                        bson_serialization_type: %s
                        serializer: foo
                        deserializer: BSONElement::fake
                    """ % (bson_type)),
                idl.errors.ERROR_ID_CUSTOM_SCALAR_SERIALIZATION_NOT_SUPPORTED)

            self.assert_bind_fail(
                textwrap.dedent("""
                types:
                    foofoo:
                        description: foo
                        cpp_type: std::string
                        bson_serialization_type: %s
                        deserializer: foo
                    """ % (bson_type)),
                idl.errors.ERROR_ID_CUSTOM_SCALAR_SERIALIZATION_NOT_SUPPORTED)

        # Test 'any' serialization needs deserializer
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: any
            """), idl.errors.ERROR_ID_MISSING_AST_REQUIRED_FIELD)

        # Test 'chain' serialization needs deserializer
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: chain
                    serializer: bar
            """), idl.errors.ERROR_ID_MISSING_AST_REQUIRED_FIELD)

        # Test 'string' serialization needs deserializer
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: string
                    serializer: bar
            """), idl.errors.ERROR_ID_MISSING_AST_REQUIRED_FIELD)

        # Test 'date' serialization needs deserializer
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: date
            """), idl.errors.ERROR_ID_MISSING_AST_REQUIRED_FIELD)

        # Test 'chain' serialization needs serializer
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: chain
                    deserializer: bar
            """), idl.errors.ERROR_ID_MISSING_AST_REQUIRED_FIELD)

        # Test list of bson types needs deserializer
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: std::int32_t
                    bson_serialization_type:
                                - int
                                - string
            """), idl.errors.ERROR_ID_MISSING_AST_REQUIRED_FIELD)

        # Test array as name
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                array<foo>:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: string
                    deserializer: bar
            """), idl.errors.ERROR_ID_ARRAY_NOT_VALID_TYPE)

    def test_struct_positive(self):
        # type: () -> None
        """Positive struct tests."""

        # Setup some common types
        test_preamble = textwrap.dedent("""
        types:
            string:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
                serializer: foo
                deserializer: foo
                default: foo
        """)

        self.assert_bind(test_preamble + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    strict: true
                    fields:
                        foo: string
            """))

    def test_struct_negative(self):
        # type: () -> None
        """Negative struct tests."""

        # Setup some common types
        test_preamble = textwrap.dedent("""
        types:
            string:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
                serializer: foo
                deserializer: foo
                default: foo
        """)

        # Test array as name
        self.assert_bind_fail(test_preamble + textwrap.dedent("""
            structs:
                array<foo>:
                    description: foo
                    strict: true
                    fields:
                        foo: string
            """), idl.errors.ERROR_ID_ARRAY_NOT_VALID_TYPE)

    def test_field_positive(self):
        # type: () -> None
        """Positive test cases for field."""

        # Setup some common types
        test_preamble = textwrap.dedent("""
        types:
            string:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
                serializer: foo
                deserializer: foo
        """)

        # Short type
        self.assert_bind(test_preamble + textwrap.dedent("""
        structs:
            bar:
                description: foo
                strict: false
                fields:
                    foo: string
            """))

        # Long type
        self.assert_bind(test_preamble + textwrap.dedent("""
        structs:
            bar:
                description: foo
                strict: false
                fields:
                    foo:
                        type: string
            """))

        # Long type with default
        self.assert_bind(test_preamble + textwrap.dedent("""
        structs:
            bar:
                description: foo
                strict: false
                fields:
                    foo:
                        type: string
                        default: bar
            """))

        # Test array as field type
        self.assert_bind(test_preamble + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    strict: true
                    fields:
                        foo: array<string>
            """))

        # Test array as field type
        self.assert_bind(
            textwrap.dedent("""
            types:
                 arrayfake:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: string
                    serializer: foo
                    deserializer: foo

            structs:
                foo:
                    description: foo
                    strict: true
                    fields:
                        arrayOfString: arrayfake
            """))

    def test_field_negative(self):
        # type: () -> None
        """Negative field tests."""

        # Setup some common types
        test_preamble = textwrap.dedent("""
        types:
            string:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
                serializer: foo
                deserializer: foo
                default: foo

            bindata:
                description: foo
                cpp_type: foo
                bson_serialization_type: bindata
                bindata_subtype: uuid
        """)

        # Test field of a struct type with a default
        self.assert_bind_fail(test_preamble + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    fields:
                        field1: string

                bar:
                    description: foo
                    fields:
                        field2:
                            type: foo
                            default: foo

            """), idl.errors.ERROR_ID_FIELD_MUST_BE_EMPTY_FOR_STRUCT)

        # Test array as field name
        self.assert_bind_fail(test_preamble + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    strict: true
                    fields:
                        array<foo>: string
            """), idl.errors.ERROR_ID_ARRAY_NOT_VALID_TYPE)

        # Test recursive array as field type
        self.assert_bind_fail(test_preamble + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    strict: true
                    fields:
                        foo: array<array<string>>
            """), idl.errors.ERROR_ID_BAD_ARRAY_TYPE_NAME)

        # Test inherited default with array
        self.assert_bind_fail(test_preamble + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    strict: true
                    fields:
                        foo: array<string>
            """), idl.errors.ERROR_ID_ARRAY_NO_DEFAULT)

        # Test non-inherited default with array
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                string:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: string
                    serializer: foo
                    deserializer: foo

            structs:
                foo:
                    description: foo
                    strict: true
                    fields:
                        foo:
                            type: array<string>
                            default: 123
            """), idl.errors.ERROR_ID_ARRAY_NO_DEFAULT)

        # Test bindata with default
        self.assert_bind_fail(test_preamble + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    strict: true
                    fields:
                        foo:
                            type: bindata
                            default: 42
            """), idl.errors.ERROR_ID_BAD_BINDATA_DEFAULT)

        # Test default and optional for the same field
        self.assert_bind_fail(test_preamble + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    strict: true
                    fields:
                        foo:
                            type: string
                            default: 42
                            optional: true
            """), idl.errors.ERROR_ID_ILLEGAL_FIELD_DEFAULT_AND_OPTIONAL)

        # Test duplicate comparison order
        self.assert_bind_fail(test_preamble + textwrap.dedent("""
        structs:
            foo:
                description: foo
                strict: false
                generate_comparison_operators: true
                fields:
                    foo:
                        type: string
                        comparison_order: 1
                    bar:
                        type: string
                        comparison_order: 1
            """), idl.errors.ERROR_ID_IS_DUPLICATE_COMPARISON_ORDER)

    def test_ignored_field_negative(self):
        # type: () -> None
        """Test that if a field is marked as ignored, no other properties are set."""
        for test_value in [
                "optional: true",
                "default: foo",
        ]:
            self.assert_bind_fail(
                textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    strict: false
                    fields:
                        foo:
                            type: string
                            ignore: true
                            %s
                """ % (test_value)), idl.errors.ERROR_ID_FIELD_MUST_BE_EMPTY_FOR_IGNORED)

    def test_chained_type_positive(self):
        # type: () -> None
        """Positive parser chaining test cases."""
        # Setup some common types
        test_preamble = textwrap.dedent("""
        types:
            string:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
                serializer: foo
                deserializer: foo
                default: foo


            foo1:
                description: foo
                cpp_type: foo
                bson_serialization_type: chain
                serializer: foo
                deserializer: foo
                default: foo

        """)

        # Chaining only
        self.assert_bind(test_preamble + textwrap.dedent("""
        structs:
            bar1:
                description: foo
                strict: false
                chained_types:
                    foo1: alias
        """))

    def test_chained_type_negative(self):
        # type: () -> None
        """Negative parser chaining test cases."""
        # Setup some common types
        test_preamble = textwrap.dedent("""
        types:
            string:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
                serializer: foo
                deserializer: foo

            foo1:
                description: foo
                cpp_type: foo
                bson_serialization_type: chain
                serializer: foo
                deserializer: foo

        """)

        # Chaining with strict struct
        self.assert_bind_fail(test_preamble + textwrap.dedent("""
        structs:
            bar1:
                description: foo
                strict: true
                chained_types:
                    foo1: alias
        """), idl.errors.ERROR_ID_CHAINED_NO_TYPE_STRICT)

        # Non-'any' type as chained type
        self.assert_bind_fail(test_preamble + textwrap.dedent("""
        structs:
            bar1:
                description: foo
                strict: false
                chained_types:
                    string: alias
        """), idl.errors.ERROR_ID_CHAINED_TYPE_WRONG_BSON_TYPE)

        # Chaining and fields only with same name
        self.assert_bind_fail(test_preamble + textwrap.dedent("""
        structs:
            bar1:
                description: foo
                strict: false
                chained_types:
                    foo1: alias
                fields:
                    foo1: string
        """), idl.errors.ERROR_ID_CHAINED_DUPLICATE_FIELD)

        # Non-existent chained type
        self.assert_bind_fail(test_preamble + textwrap.dedent("""
        structs:
            bar1:
                description: foo
                strict: false
                chained_types:
                    foobar1: alias
                fields:
                    foo1: string
        """), idl.errors.ERROR_ID_UNKNOWN_TYPE)

        # A regular field as a chained type
        self.assert_bind_fail(test_preamble + textwrap.dedent("""
        structs:
            bar1:
                description: foo
                strict: false
                fields:
                    foo1: string
                    foo2: foobar1
        """), idl.errors.ERROR_ID_UNKNOWN_TYPE)

        # Array of chained types
        self.assert_bind_fail(test_preamble + textwrap.dedent("""
        structs:
            bar1:
                description: foo
                strict: true
                fields:
                    field1: array<foo1>
        """), idl.errors.ERROR_ID_NO_ARRAY_OF_CHAIN)

    def test_chained_struct_positive(self):
        # type: () -> None
        """Positive parser chaining test cases."""
        # Setup some common types
        test_preamble = textwrap.dedent("""
        types:
            string:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
                serializer: foo
                deserializer: foo
                default: foo


            foo1:
                description: foo
                cpp_type: foo
                bson_serialization_type: chain
                serializer: foo
                deserializer: foo
                default: foo

        structs:
            chained:
                description: foo
                strict: false
                chained_types:
                    foo1: alias

            chained2:
                description: foo
                strict: false
                fields:
                    field1: string
        """)

        # A struct with only chaining
        self.assert_bind(test_preamble + indent_text(1,
                                                     textwrap.dedent("""
            bar1:
                description: foo
                strict: true
                chained_structs:
                    chained2: alias
        """)))

        # Chaining struct's fields and explicit fields
        self.assert_bind(test_preamble + indent_text(1,
                                                     textwrap.dedent("""
            bar1:
                description: foo
                strict: true
                chained_structs:
                    chained2: alias
                fields:
                    str1: string
        """)))

        # Chained types and structs
        self.assert_bind(test_preamble + indent_text(1,
                                                     textwrap.dedent("""
            bar1:
                description: foo
                strict: false
                chained_types:
                    foo1: alias
                chained_structs:
                    chained2: alias
                fields:
                    str1: string
        """)))

        # Non-strict chained struct
        self.assert_bind(test_preamble + indent_text(1,
                                                     textwrap.dedent("""
            bar1:
                description: foo
                strict: false
                chained_structs:
                    chained2: alias
                fields:
                    foo1: string
        """)))

        # Inline Chained struct with strict true
        self.assert_bind(test_preamble + indent_text(1,
                                                     textwrap.dedent("""
            bar1:
                description: foo
                strict: true
                fields:
                    field1: string

            foobar:
                description: foo
                strict: false
                inline_chained_structs: true
                chained_structs:
                    bar1: alias
                fields:
                    f1: string

        """)))

        # Inline Chained struct with strict true and inline_chained_structs defaulted
        self.assert_bind(test_preamble + indent_text(1,
                                                     textwrap.dedent("""
            bar1:
                description: foo
                strict: true
                fields:
                    field1: string

            foobar:
                description: foo
                strict: false
                chained_structs:
                    bar1: alias
                fields:
                    f1: string
        """)))

    def test_chained_struct_negative(self):
        # type: () -> None
        """Negative parser chaining test cases."""
        # Setup some common types
        test_preamble = textwrap.dedent("""
        types:
            string:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
                serializer: foo
                deserializer: foo
                default: foo


            foo1:
                description: foo
                cpp_type: foo
                bson_serialization_type: chain
                serializer: foo
                deserializer: foo
                default: foo

        structs:
            chained:
                description: foo
                strict: false
                fields:
                    field1: string

            chained2:
                description: foo
                strict: false
                fields:
                    field1: string
        """)

        # Non-existing chained struct
        self.assert_bind_fail(test_preamble + indent_text(1,
                                                          textwrap.dedent("""
            bar1:
                description: foo
                strict: true
                chained_structs:
                    foobar1: alias
        """)), idl.errors.ERROR_ID_UNKNOWN_TYPE)

        # Type as chained struct
        self.assert_bind_fail(test_preamble + indent_text(1,
                                                          textwrap.dedent("""
            bar1:
                description: foo
                strict: true
                chained_structs:
                    foo1: alias
        """)), idl.errors.ERROR_ID_CHAINED_STRUCT_NOT_FOUND)

        # Struct as chained type
        self.assert_bind_fail(test_preamble + indent_text(1,
                                                          textwrap.dedent("""
            bar1:
                description: foo
                strict: false
                chained_types:
                    chained: alias
        """)), idl.errors.ERROR_ID_CHAINED_TYPE_NOT_FOUND)

        # Duplicated field names across chained struct's fields and fields
        self.assert_bind_fail(test_preamble + indent_text(1,
                                                          textwrap.dedent("""
            bar1:
                description: foo
                strict: false
                chained_structs:
                    chained: alias
                fields:
                    field1: string
        """)), idl.errors.ERROR_ID_CHAINED_DUPLICATE_FIELD)

        # Duplicated field names across chained structs
        self.assert_bind_fail(test_preamble + indent_text(1,
                                                          textwrap.dedent("""
            bar1:
                description: foo
                strict: false
                chained_structs:
                    chained: alias
                    chained2: alias
        """)), idl.errors.ERROR_ID_CHAINED_DUPLICATE_FIELD)

        # Chained struct with strict true
        self.assert_bind_fail(test_preamble + indent_text(1,
                                                          textwrap.dedent("""
            bar1:
                description: foo
                strict: true
                fields:
                    field1: string

            foobar:
                description: foo
                strict: false
                inline_chained_structs: false
                chained_structs:
                    bar1: alias
                fields:
                    f1: string

        """)), idl.errors.ERROR_ID_CHAINED_NO_NESTED_STRUCT_STRICT)

        # Chained struct with nested chained struct
        self.assert_bind_fail(test_preamble + indent_text(1,
                                                          textwrap.dedent("""
            bar1:
                description: foo
                strict: false
                chained_structs:
                    chained: alias

            foobar:
                description: foo
                strict: false
                chained_structs:
                    bar1: alias
                fields:
                    f1: string

        """)), idl.errors.ERROR_ID_CHAINED_NO_NESTED_CHAINED)

        # Chained struct with nested chained type
        self.assert_bind_fail(test_preamble + indent_text(1,
                                                          textwrap.dedent("""
            bar1:
                description: foo
                strict: false
                chained_types:
                    foo1: alias

            foobar:
                description: foo
                strict: false
                chained_structs:
                    bar1: alias
                fields:
                    f1: bar1

        """)), idl.errors.ERROR_ID_CHAINED_NO_NESTED_CHAINED)

    def test_enum_positive(self):
        # type: () -> None
        """Positive enum test cases."""

        # Test int
        self.assert_bind(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: int
                values:
                    v1: 3
                    v2: 1
                    v3: 2
            """))

        # Test string
        self.assert_bind(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: string
                values:
                    v1: 0
                    v2: 1
                    v3: 2
            """))

    def test_enum_negative(self):
        # type: () -> None
        """Negative enum test cases."""

        # Test wrong type
        self.assert_bind_fail(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: foo
                values:
                    v1: 0
            """), idl.errors.ERROR_ID_ENUM_BAD_TYPE)

        # Test int - non continuous
        self.assert_bind_fail(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: int
                values:
                    v1: 0
                    v3: 2
            """), idl.errors.ERROR_ID_ENUM_NON_CONTINUOUS_RANGE)

        # Test int - dups
        self.assert_bind_fail(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: int
                values:
                    v1: 1
                    v3: 1
            """), idl.errors.ERROR_ID_ENUM_NON_UNIQUE_VALUES)

        # Test int - non-integer value
        self.assert_bind_fail(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: int
                values:
                    v1: foo
                    v3: 1
            """), idl.errors.ERROR_ID_ENUM_BAD_INT_VAUE)

        # Test string - dups
        self.assert_bind_fail(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: string
                values:
                    v1: foo
                    v3: foo
            """), idl.errors.ERROR_ID_ENUM_NON_UNIQUE_VALUES)

    def test_struct_enum_negative(self):
        # type: () -> None
        """Negative enum test cases."""

        test_preamble = textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: int
                values:
                    v1: 0
                    v2: 1
        """)

        # Test array of enums
        self.assert_bind_fail(test_preamble + textwrap.dedent("""
        structs:
            foo1:
                description: foo
                fields:
                    foo1: array<foo>
            """), idl.errors.ERROR_ID_NO_ARRAY_ENUM)

    def test_command_positive(self):
        # type: () -> None
        """Positive command tests."""

        # Setup some common types
        test_preamble = textwrap.dedent("""
        types:
            string:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
                serializer: foo
                deserializer: foo
                default: foo
        """)

        self.assert_bind(test_preamble + textwrap.dedent("""
            commands:
                foo:
                    description: foo
                    namespace: ignored
                    strict: true
                    fields:
                        foo1: string
            """))

    def test_command_negative(self):
        # type: () -> None
        """Negative command tests."""

        # Setup some common types
        test_preamble = textwrap.dedent("""
        types:
            string:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
                serializer: foo
                deserializer: foo
                default: foo
        """)

        # Commands cannot be fields in other commands
        self.assert_bind_fail(test_preamble + textwrap.dedent("""
            commands:
                foo:
                    description: foo
                    namespace: ignored
                    fields:
                        foo1: string

                bar:
                    description: foo
                    namespace: ignored
                    fields:
                        foo: foo
            """), idl.errors.ERROR_ID_FIELD_NO_COMMAND)

        # Commands cannot be fields in structs
        self.assert_bind_fail(test_preamble + textwrap.dedent("""
            commands:
                foo:
                    description: foo
                    namespace: ignored
                    fields:
                        foo1: string

            structs:
                bar:
                    description: foo
                    fields:
                        foo: foo
            """), idl.errors.ERROR_ID_FIELD_NO_COMMAND)

        # Commands cannot have a field as the same name
        self.assert_bind_fail(test_preamble + textwrap.dedent("""
            commands:
                foo:
                    description: foo
                    namespace: ignored
                    fields:
                        foo: string
            """), idl.errors.ERROR_ID_COMMAND_DUPLICATES_FIELD)

    def test_command_doc_sequence_positive(self):
        # type: () -> None
        """Positive supports_doc_sequence tests."""
        # pylint: disable=invalid-name

        # Setup some common types
        test_preamble = textwrap.dedent("""
        types:
            object:
                description: foo
                cpp_type: foo
                bson_serialization_type: object
                serializer: foo
                deserializer: foo

            string:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
                serializer: foo
                deserializer: foo

        structs:
            foo_struct:
                description: foo
                strict: true
                fields:
                    foo: object
        """)

        self.assert_bind(test_preamble + textwrap.dedent("""
            commands:
                foo:
                    description: foo
                    namespace: ignored
                    fields:
                        foo1:
                            type: array<object>
                            supports_doc_sequence: true
            """))

        self.assert_bind(test_preamble + textwrap.dedent("""
            commands:
                foo:
                    description: foo
                    namespace: ignored
                    fields:
                        foo1:
                            type: array<foo_struct>
                            supports_doc_sequence: true
            """))

    def test_command_doc_sequence_negative(self):
        # type: () -> None
        """Negative supports_doc_sequence tests."""
        # pylint: disable=invalid-name

        # Setup some common types
        test_preamble = textwrap.dedent("""
        types:
            object:
                description: foo
                cpp_type: foo
                bson_serialization_type: object
                serializer: foo
                deserializer: foo

            string:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
                serializer: foo
                deserializer: foo

            any_type:
                description: foo
                cpp_type: foo
                bson_serialization_type: any
                serializer: foo
                deserializer: foo
        """)

        test_preamble2 = test_preamble + textwrap.dedent("""
        structs:
            foo_struct:
                description: foo
                strict: true
                fields:
                    foo: object
        """)

        # A struct
        self.assert_bind_fail(test_preamble + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    fields:
                        foo:
                            type: array<object>
                            supports_doc_sequence: true
            """), idl.errors.ERROR_ID_STRUCT_NO_DOC_SEQUENCE)

        # A non-array type
        self.assert_bind_fail(test_preamble + textwrap.dedent("""
            commands:
                foo:
                    description: foo
                    namespace: ignored
                    fields:
                        foo:
                            type: object
                            supports_doc_sequence: true
            """), idl.errors.ERROR_ID_NO_DOC_SEQUENCE_FOR_NON_ARRAY)

        # An array of a scalar
        self.assert_bind_fail(test_preamble2 + textwrap.dedent("""
            commands:
                foo:
                    description: foo
                    namespace: ignored
                    fields:
                        foo1:
                            type: array<string>
                            supports_doc_sequence: true
            """), idl.errors.ERROR_ID_NO_DOC_SEQUENCE_FOR_NON_OBJECT)

        # An array of 'any'
        self.assert_bind_fail(test_preamble2 + textwrap.dedent("""
            commands:
                foo:
                    description: foo
                    namespace: ignored
                    fields:
                        foo1:
                            type: array<string>
                            supports_doc_sequence: true
            """), idl.errors.ERROR_ID_NO_DOC_SEQUENCE_FOR_NON_OBJECT)

    def test_command_type_positive(self):
        # type: () -> None
        """Positive command custom type test cases."""
        test_preamble = textwrap.dedent("""
        types:
            string:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
                serializer: foo
                deserializer: foo
        """)

        # string
        self.assert_bind(test_preamble + textwrap.dedent("""
        commands:
            foo:
                description: foo
                strict: true
                namespace: type
                type: string
                fields:
                    field1: string
            """))

        # array of string
        self.assert_bind(test_preamble + textwrap.dedent("""
        commands:
            foo:
                description: foo
                strict: true
                namespace: type
                type: array<string>
                fields:
                    field1: string
            """))

    def test_command_type_negative(self):
        # type: () -> None
        """Negative command type test cases."""
        test_preamble = textwrap.dedent("""
        types:
            string:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
                serializer: foo
                deserializer: foo
        """)

        # supports_doc_sequence must be a bool
        self.assert_bind_fail(test_preamble + textwrap.dedent("""
        commands:
            foo:
                description: foo
                namespace: type
                type: int
                fields:
                    field1: string
            """), idl.errors.ERROR_ID_UNKNOWN_TYPE)


if __name__ == '__main__':

    unittest.main()
