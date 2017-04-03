package Google::ProtocolBuffers::Constants;
use strict;
use warnings;

my ($types, $wiretypes, $labels, $complex_types);
BEGIN 
{
    ## from src/google/protobuf/descriptor.h
    $types = {
        TYPE_DOUBLE         => 1,   ## double, exactly eight bytes on the wire.
        TYPE_FLOAT          => 2,   ## float, exactly four bytes on the wire.
        TYPE_INT64          => 3,   ## int64, varint on the wire.  Negative numbers
                                    ## take 10 bytes.  Use TYPE_SINT64 if negative
                                    ## values are likely.
        TYPE_UINT64         => 4,   ## uint64, varint on the wire.
        TYPE_INT32          => 5,   ## int32, varint on the wire.  Negative numbers
                                    ## take 10 bytes.  Use TYPE_SINT32 if negative
                                    ## values are likely.
        TYPE_FIXED64        => 6,   ## uint64, exactly eight bytes on the wire.
        TYPE_FIXED32        => 7,   ## uint32, exactly four bytes on the wire.
        TYPE_BOOL           => 8,   ## bool, varint on the wire.
        TYPE_STRING         => 9,   ## UTF-8 text.
        TYPE_GROUP          => 10,  ## Tag-delimited message.  Deprecated.
        TYPE_MESSAGE        => 11,  ## Length-delimited message.
        TYPE_BYTES          => 12,  ## Arbitrary byte array.
        TYPE_UINT32         => 13,  ## uint32, varint on the wire
        TYPE_ENUM           => 14,  ## Enum, varint on the wire
        TYPE_SFIXED32       => 15,  ## int32, exactly four bytes on the wire
        TYPE_SFIXED64       => 16,  ## int64, exactly eight bytes on the wire
        TYPE_SINT32         => 17,  ## int32, ZigZag-encoded varint on the wire
        TYPE_SINT64         => 18,  ## int64, ZigZag-encoded varint on the wire
    };

    ## from src/google/protobuf/descriptor.h 
    $labels = {
        LABEL_OPTIONAL      => 1,
        LABEL_REQUIRED      => 2,
        LABEL_REPEATED      => 3,
    };


    ## from src/google/protobuf/wire_format.h 
    $wiretypes = {
        WIRETYPE_VARINT           => 0,
        WIRETYPE_FIXED64          => 1,
        WIRETYPE_LENGTH_DELIMITED => 2,
        WIRETYPE_START_GROUP      => 3,
        WIRETYPE_END_GROUP        => 4,
        WIRETYPE_FIXED32          => 5,
    };


    ## Complex types - this is not a part of Google specificaion
    $complex_types =  {
        MESSAGE => 1,
        GROUP   => 2,
        ENUM    => 3,
        ONEOF   => 4,
    };
}

use base 'Exporter';
use vars qw/@EXPORT_OK %EXPORT_TAGS/;

use constant $types;
$EXPORT_TAGS{'types'} = [keys %$types];
push @{$EXPORT_TAGS{'all'}}, keys %$types;
push @EXPORT_OK, keys %$types;

use constant $wiretypes;
$EXPORT_TAGS{'wiretypes'} = [keys %$wiretypes];
push @{$EXPORT_TAGS{'all'}}, keys %$wiretypes;
push @EXPORT_OK, keys %$wiretypes;

use constant $labels;
$EXPORT_TAGS{'labels'} = [keys %$labels];
push @{$EXPORT_TAGS{'all'}}, keys %$labels;
push @EXPORT_OK, keys %$labels;

use constant $complex_types;
$EXPORT_TAGS{'complex_types'} = [keys %$complex_types];
push @{$EXPORT_TAGS{'all'}}, keys %$complex_types;
push @EXPORT_OK, keys %$complex_types;

1;

