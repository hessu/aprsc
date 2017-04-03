package Google::ProtocolBuffers::Codec;
use strict;
use warnings;
## FATAL substr warnings ("substring outside of string") was intended
## to report about incomplete messages.
## However, substr("abc", 3, 1) returns chr(0) without warning.
## Thats why the code below has to check length of string and 
## substring index manually
use warnings FATAL => 'substr';

use Config qw/%Config/;
use Google::ProtocolBuffers::Constants qw/:all/;
use Encode ();

use constant BROKEN_MESSAGE => "Mesage is incomplete or invalid";
use constant MAX_UINT32 => 0xffff_ffff;
use constant MAX_SINT32 => 0x7fff_ffff;
use constant MIN_SINT32 =>-0x8000_0000;

BEGIN {
    ## Protocol Buffer standard requires support of 64-bit integers.
    ## If platform doen't support them internally, they will be emulated
    ## by Math::BigInt number.
    ## Libraries below contains identically named funtions that are either
    ## use native 64-bit ints or Math::BigInts 
    my $ivsize = $Config{ivsize};
    if ($ivsize>=8) {
        require 'Google/ProtocolBuffers/CodecIV64.pm';
    } elsif ($ivsize==4) {
        require 'Google/ProtocolBuffers/CodecIV32.pm';
    } else {
        die "Unsupported size of internal Perl IntegerValue: '$ivsize' bytes.";
    }
}

BEGIN {
    ## Floats and doubles are packed in their native format,
    ## which is different on big-endian and litte-endian platforms
    ## Maybe create and load one of two files, like CodecIV* above?
    my $bo = $Config{byteorder}; 
    if ($bo =~ '^1234') {
        ## little-endian platform
        *encode_float  = \&encode_float_le; 
        *decode_float  = \&decode_float_le; 
        *encode_double = \&encode_double_le; 
        *decode_double = \&decode_double_le; 
    } elsif ($bo =~ '4321$') {
        ## big-endian
        *encode_float  = \&encode_float_be; 
        *decode_float  = \&decode_float_be; 
        *encode_double = \&encode_double_be; 
        *decode_double = \&decode_double_be; 
    }
}

my @primitive_type_encoders;
$primitive_type_encoders[TYPE_DOUBLE]   = \&encode_double;
$primitive_type_encoders[TYPE_FLOAT]    = \&encode_float;
$primitive_type_encoders[TYPE_INT64]    = \&encode_int;
$primitive_type_encoders[TYPE_UINT64]   = \&encode_uint;
$primitive_type_encoders[TYPE_INT32]    = \&encode_int;
$primitive_type_encoders[TYPE_FIXED64]  = \&encode_fixed64;
$primitive_type_encoders[TYPE_FIXED32]  = \&encode_fixed32;
$primitive_type_encoders[TYPE_BOOL]     = \&encode_bool;
$primitive_type_encoders[TYPE_STRING]   = \&encode_string;
$primitive_type_encoders[TYPE_BYTES]    = \&encode_string;
$primitive_type_encoders[TYPE_UINT32]   = \&encode_uint;
$primitive_type_encoders[TYPE_ENUM]     = \&encode_int;
$primitive_type_encoders[TYPE_SFIXED64] = \&encode_sfixed64;
$primitive_type_encoders[TYPE_SFIXED32] = \&encode_sfixed32;
$primitive_type_encoders[TYPE_SINT32]   = \&encode_sint;
$primitive_type_encoders[TYPE_SINT64]   = \&encode_sint;

my @primitive_type_decoders;
$primitive_type_decoders[TYPE_DOUBLE]   = \&decode_double;
$primitive_type_decoders[TYPE_FLOAT]    = \&decode_float;
$primitive_type_decoders[TYPE_INT64]    = \&decode_int;
$primitive_type_decoders[TYPE_UINT64]   = \&decode_uint;
$primitive_type_decoders[TYPE_INT32]    = \&decode_int;
$primitive_type_decoders[TYPE_FIXED64]  = \&decode_fixed64;
$primitive_type_decoders[TYPE_FIXED32]  = \&decode_fixed32;
$primitive_type_decoders[TYPE_BOOL]     = \&decode_bool;
$primitive_type_decoders[TYPE_STRING]   = \&decode_string;
$primitive_type_decoders[TYPE_BYTES]    = \&decode_string;
$primitive_type_decoders[TYPE_UINT32]   = \&decode_uint;
$primitive_type_decoders[TYPE_ENUM]     = \&decode_int;
$primitive_type_decoders[TYPE_SFIXED64] = \&decode_sfixed64;
$primitive_type_decoders[TYPE_SFIXED32] = \&decode_sfixed32;
$primitive_type_decoders[TYPE_SINT32]   = \&decode_sint;
$primitive_type_decoders[TYPE_SINT64]   = \&decode_sint;

my @wire_types;
$wire_types[TYPE_DOUBLE]    = WIRETYPE_FIXED64;
$wire_types[TYPE_FLOAT]     = WIRETYPE_FIXED32;
$wire_types[TYPE_INT64]     = WIRETYPE_VARINT; 
$wire_types[TYPE_UINT64]    = WIRETYPE_VARINT;
$wire_types[TYPE_INT32]     = WIRETYPE_VARINT;
$wire_types[TYPE_FIXED64]   = WIRETYPE_FIXED64;
$wire_types[TYPE_FIXED32]   = WIRETYPE_FIXED32;
$wire_types[TYPE_BOOL]      = WIRETYPE_VARINT; 
$wire_types[TYPE_STRING]    = WIRETYPE_LENGTH_DELIMITED; 
## these types were removed deliberatly from the list,
## since they must be serialized by their own classes 
##$wire_types[TYPE_GROUP]   
##$wire_types[TYPE_MESSAGE] 
$wire_types[TYPE_BYTES]     = WIRETYPE_LENGTH_DELIMITED;
$wire_types[TYPE_UINT32]    = WIRETYPE_VARINT;
## we create a special class for each enum, but these classes
## are just namespaces for constants. User can create a message
## field with type=TYPE_ENUM and integer value.
$wire_types[TYPE_ENUM]      = WIRETYPE_VARINT; 
$wire_types[TYPE_SFIXED32]  = WIRETYPE_FIXED32;
$wire_types[TYPE_SFIXED64]  = WIRETYPE_FIXED64; 
$wire_types[TYPE_SINT32]    = WIRETYPE_VARINT;
$wire_types[TYPE_SINT64]    = WIRETYPE_VARINT;


##
## Class or instance method. 
## Must not be called directly, only as a method of derived class.
##
## Input: data structure (hash-ref)
## Output: in-memory string with serialized data
##
## Example: 
##      my $str = My::Message->encode({a => 1});
## or 
##      my $message = bless {a => 1}, 'My::Message';
##      my $str = $message->encode;
##
sub encode 
{
    my $self = shift;
    my $data = (ref $self) ? $self : shift();
    
    ##unless (ref $data eq 'HASH') {
    ##  my $class = ref $self || $self;
    ##    die "Hashref was expected for $self->encode; found '$data' instead";        
    ##}

    my $buf = '';
    foreach my $field (@{ $self->_pb_fields_list }) {
        my ($cardinality, $type, $name, $field_number, $default) = @$field;
        ## Check mising values and their cardinality (i.e. label): required, optional or repeated.
        ## For required fields, put a default value into stream, if exists, and raise an error otherwise.
        my $value = $data->{$name};
        if (!defined $value) {
            if ($cardinality==LABEL_REQUIRED) {
                if (defined $default) {
                    $value = $default;
                } else {
                    die "Required field '$name' is missing in $self";
                }
            } else {
                next;
            }
        } 
        
        if (ref $value && ref $value eq 'ARRAY') {
            if ($cardinality!=LABEL_REPEATED) {
                ## Oops, several values were given for a non-repeated field.
                ## We'll take the last one - the specification states that
                ## if several (non-repeaded) fields are in a stream,
                ## the last one must be taken
                $value = $value->[-1];
            }
        }
        my $is_repeated = ref $value && ref $value eq 'ARRAY';
        
        $field_number <<= 3;

        no warnings 'numeric';
        my $encoder = $primitive_type_encoders[$type];
        use warnings;

        if ($encoder) {
            ##
            ## this field is one of the base types
            ##
            die $type unless exists $wire_types[$type];
            if (!$is_repeated) {
                encode_varint($buf, $field_number | $wire_types[$type]);
                $encoder->($buf, $value);
            } else {
                my $key; 
                encode_varint($key, $field_number | $wire_types[$type]);
                foreach my $v (@$value) {
                    $buf .= $key;
                    $encoder->($buf, $v);
                }
            }
        } else {
            ##
            ## This field is one of complex types: another message, group or enum
            ## 
            my $kind = $type->_pb_complex_type_kind;
            if ($kind==MESSAGE) {
                if (!$is_repeated) {
                    encode_varint($buf, $field_number | WIRETYPE_LENGTH_DELIMITED);
                    my $message = $type->encode($value);
                    encode_varint($buf, length($message));
                    $buf .= $message;
                } else {
                    my $key;
                    encode_varint($key, $field_number | WIRETYPE_LENGTH_DELIMITED);
                    foreach my $v (@$value) {
                        $buf .= $key;
                        my $message = $type->encode($v);
                        encode_varint($buf, length($message));
                        $buf .= $message;
                    }
                }
            }
            elsif ($kind==ENUM) {
                if (!$is_repeated) { 
                    encode_varint($buf, $field_number | WIRETYPE_VARINT);
                    encode_int($buf, $value);
                } else {
                    my $key; 
                    encode_varint($key, $field_number | WIRETYPE_VARINT);
                    foreach my $v (@$value) {
                        $buf .= $key;
                        encode_int($buf, $v);
                    }
                }
            }
            elsif ($kind==GROUP) {
                if (!$is_repeated) { 
                    encode_varint($buf, $field_number | WIRETYPE_START_GROUP);
                    $buf .= encode($type, $value);
                    encode_varint($buf, $field_number | WIRETYPE_END_GROUP);
                } else {
                    my ($start,$end);
                    encode_varint($start, $field_number | WIRETYPE_START_GROUP);
                    encode_varint($end,   $field_number | WIRETYPE_END_GROUP);
                    foreach my $v (@$value) {
                        $buf .= $start;
                        $buf .= encode($type, $v);
                        $buf .= $end;
                    }
                }
            } else {
                die "Unkown type: $type ($kind)";
            }
        }
    }
    return $buf;    
}

##
## Class method.
## Must not be called directly, only as a method of derived class
##
## Input: string of serialized data
## Output: data structure (hashref)
## If serialized data contains errors, an exception will be thrown.
##
## Example:
##      my $data = My::Message->decode($str);
##      ## $data is now a hashref like this: {a => 1}
##   
sub decode {
    my $class = shift;
    
    ## position must be a modifiable variable (it's passed by reference
    ## to all decode subroutines, that call each other recursively)
    ## It's slightly quicker then passing it as an object attribute 
    ## ($self->{pos}) to each method, but readability is poor. 
    my $pos = 0;
    if (Encode::is_utf8($_[0])) {
        ## oops, wide-character string, where did you get it from?
        ## Should we silently encode it to utf-8 and then process
        ## the resulted byte-string?
        die "Input data string is a wide-character string";
    }
    return _decode_partial($class, $_[0], $pos, length($_[0]));
}

##
## Internal method, decodes both Messages and Groups
## Input:   
##  data string, 
##  start_position (passed by reference, this must be a variable), 
##  length of message
## Output: 
##  for Messages: data structure 
##  for Groups: (data structure, field number of ending group tag)
##
sub _decode_partial {
    my $class = shift;
    
    my $length = $_[2];
    my $end_position = $_[1]+$length;

    my $data = bless {}, $class;
    my $fields = $class->_pb_fields_by_number;

    PAIR:
    while ($_[1] < $end_position) {
        my $v = decode_varint($_[0], $_[1]);
        my ($field_number, $wire_type) = ($v>>3, $v&7);

        if ($wire_type==WIRETYPE_END_GROUP) {
            if ($class->_pb_complex_type_kind==GROUP) {
                return ($data, $field_number);
            } else {
                die "Unexpected end of group in message";
            }
        }
        
        if (my $field = $fields->{$field_number}) {
            my ($cardinality, $type, $name, $field_number_, $default) = @$field;
            die unless $field_number_== $field_number;
            my $value;

            no warnings 'numeric';
            my $decoder = $primitive_type_decoders[$type];
            use warnings;

            if ($decoder) {
                if ($wire_type==WIRETYPE_LENGTH_DELIMITED && $type!=TYPE_STRING && $type!=TYPE_BYTES) {
                    ##
                    ## Packed Repeated Fields:
                    ## <length of the field>; sequence of encoded <primitive values>
                    ##
                    ## order is important - $_[1] changed by decode_varint()
                    my $l = decode_varint($_[0], $_[1]);    ## length of the packed field
                    my $e = $_[1] + $l;                     ## last position of the field

                    my @values;
                    while ($_[1]<$e) {
                        push @values, $decoder->($_[0], $_[1]);
                    }
                    if ($cardinality==LABEL_REPEATED) {
                        push @{$data->{$name}}, @values;
                    } else {
                        $data->{$name} = $values[-1];
                    }
                    next PAIR;

                } else {
                    ## regular primitive value, string or byte array
                    $value = $decoder->($_[0], $_[1]);
                }
            } else {
                my $kind = $type->_pb_complex_type_kind;
                if ($kind==MESSAGE) {
                    my $message_length = decode_varint($_[0], $_[1]);
                    $value = _decode_partial($type, $_[0], $_[1], $message_length); 
                } elsif ($kind==ENUM) {
                    $value = decode_int($_[0], $_[1]);
                } elsif ($kind==GROUP) {
                    my $end_field_number;
                    ($value, $end_field_number) = _decode_partial($type, $_[0], $_[1], $end_position-$_[1]);
                    die unless $field_number == $end_field_number;
                } else {
                    die "Unkown type: $type ($kind)";
                }
            }
            if ($cardinality==LABEL_REPEATED) {
                push @{$data->{$name}}, $value;
            } else {
                $data->{$name} = $value;
            }
        }
        else {
            _skip_unknown_field($_[0], $_[1], $field_number, $wire_type);
        }
    }
    
    if ($class->_pb_complex_type_kind==GROUP) {
        die "End of group token was not found";
    } else {
        return $data;
    }
}

##
## Subroutines for skipping unknown fields 
##
## _skip_unknown_field($buffer, $position, $field_number, $wire_type)
##      $buffer is immutable
##      $position will be advanced
##      $field_number is for groups only, and for checks that closing group 
##          field_number equals to the (given) opening field_number
##      $wire_type is to know lenght of field to be skipped
##  Returns none
##
sub _skip_unknown_field {
    my ($field_number, $wire_type) = ($_[2], $_[3]);
                
    if ($wire_type==WIRETYPE_VARINT) {
        _skip_varint($_[0], $_[1]);
    } elsif ($wire_type==WIRETYPE_FIXED64) {
        $_[1] += 8;
    } elsif ($wire_type==WIRETYPE_LENGTH_DELIMITED) {
        my $len = decode_varint($_[0], $_[1]);
        $_[1] += $len;
    } elsif ($wire_type==WIRETYPE_START_GROUP) {
        my $closing_field_number = _skip_until_end_of_group($_[0], $_[1]);
        die unless $closing_field_number==$field_number;
    } elsif ($wire_type==WIRETYPE_END_GROUP) {
        die "Unexpected end of group";
    } elsif ($wire_type==WIRETYPE_FIXED32) {
        $_[1] += 4;
    } else {
        die "Unknown wire type $wire_type";
    }
}

##
## _skip_until_end_of_group($buffer, $position);
## Returns field_number of closing group tag
##
sub _skip_until_end_of_group {
    while (1) {
        my $v = decode_varint($_[0], $_[1]);
        my ($field_number, $wire_type) = ($v>>3, $v&7);
        return $field_number if $wire_type==WIRETYPE_END_GROUP;
        _skip_unknown_field($_[0], $_[1], $field_number, $wire_type);
    }
}

##
## _skip_varint($buffer, $position)
## Returns none
sub _skip_varint {
    my $c = 0;
    my $l = length($_[0]);
    while (1) {
        die BROKEN_MESSAGE() if $_[1] >= $l; ## if $_[1]+1 > $l
        last if (ord(substr($_[0], $_[1]++, 1)) & 0x80) == 0;
        die "Varint is too long" if ++$c>=9;
    }
}

##
## Implementations of primitive types serialization/deserialization are
## below. Some of subroutines are defined in IV32/IV64 modules.
##
## Signature of all encode_* subs:
##      encode_*($buffer, $value);
## Encoded value of $value will be appended to $buffer, which is a string
## passed by reference. No meaningfull value is returned, in case of errors
## an exception it thrown.
## 
## Signature of all encode_* subs:
##      my $value = decode_*($buffer, $position);
## $buffer is a string passed by reference, no copy is performed and it
## is not modified. $position is a number variable passed by reference
## (index in the string $buffer where to start decoding of a value), it
## is incremented by decode_* subs. In case of errors an exception is
## thrown.
##
## Sorry for poor readability, these subroutines were optimized for speed.
## Most probably, they (and this module entirely) should be written in XS
##

##
## type: varint
##
## Our implementation of varint knows about positive numbers only.
## It's caller's responsibility to convert negative values into 
## 64-bit positives
##
sub encode_varint {
    my $v = $_[1];
    die "Varint is negative" if $v < 0;
    my $c = 0;
    while ($v > 0x7F) {
        $_[0] .= chr( ($v&0x7F) | 0x80 );
        $v >>= 7;
        die "Number is too long" if ++$c >= 10;
    }
    $_[0] .= chr( ($v&0x7F) );
}
## sub decode_varint - word-size sensitive

##
## type: unsigend int (32/64)
##
## sub encode_uint - word-size sensitive
*encode_uint = \&encode_int;    

## decode_varint always returns positive value
sub decode_uint {
    return decode_varint(@_);
}

##
## type: signed int (32/64)
##
## Signed zigzag-encode integers
## Acutally, zigzag encoded value is just ($v>0) ? $v*2 : (-$v)*2-1;
##

sub decode_sint {
    my $v = decode_varint(@_);
    if ($v & 1) {
        ## warning: -(($v+1)>>1) may cause overflow
        return -(1 + (($v-1)>>1))
    } else {
        return $v>>1;
    }
}

##
## type: boolean
##
sub encode_bool {
    if ($_[1]) {
        encode_varint($_[0], 1);
    } else {
        encode_varint($_[0], 0);
    }
}

sub decode_bool {
    return (decode_varint(@_)) ? 1 : 0;
}

##
## type: unsigned fixed 64-bit int
##
##sub encode_fixed64 - word-size sensitive
##sub decode_fixed64 - word-size sensitive

##
## type: signed fixed 64-bit int
##
##sub encode_sfixed64 - word-size sensitive
##sub decode_sfixed64 - word-size sensitive

##
## type: double
##
## little-endian versions
sub encode_double_le {
    $_[0] .= pack('d', $_[1]);
}
sub decode_double_le {
    die BROKEN_MESSAGE() if $_[1]+8 > length($_[0]); 
    my $v = unpack('d', substr($_[0], $_[1], 8));
    $_[1] += 8;
    return $v;
}

## big-endian versions
sub encode_double_be {
    $_[0] .= reverse pack('d', $_[1]);
}
sub decode_double_be {
    die BROKEN_MESSAGE() if $_[1]+8 > length($_[0]); 
    my $v = unpack('d', reverse substr($_[0], $_[1], 8));
    $_[1] += 8;
    return $v;
}

##
## type: string and bytes
##
sub encode_string {
    use Carp; Carp::cluck("Undefined string") unless defined $_[1];
    if (Encode::is_utf8($_[1])) {
        ## Ops, the string has wide-characters.
        ## Well, encode them to utf-8 bytes.
        my $v = Encode::encode_utf8($_[1]);
        encode_varint($_[0], length($v));
        $_[0] .= $v;
    } else {
        encode_varint($_[0], length($_[1]));
        $_[0] .= $_[1];
    }
}

sub decode_string {
    my $length = decode_varint(@_);
    die BROKEN_MESSAGE() if $_[1]+$length > length($_[0]); 
    my $str = substr($_[0], $_[1], $length);
    $_[1] += $length;
    return $str;
}

##
## type: unsigned 32-bit
##
sub encode_fixed32 {
    $_[0] .= pack('V', $_[1]);
}
sub decode_fixed32 {
    die BROKEN_MESSAGE() if $_[1]+4 > length($_[0]); 
    my $v = unpack('V', substr($_[0], $_[1], 4));
    $_[1] += 4; 
    return $v;
}

##
## type: signed 32-bit
##
sub encode_sfixed32 {
    $_[0] .= pack('V', $_[1]);
}
sub decode_sfixed32 {
    die BROKEN_MESSAGE() if $_[1]+4 > length($_[0]); 
    my $v = unpack('V', substr($_[0], $_[1], 4));
    $_[1] += 4; 
    return ($v>MAX_SINT32()) ? ($v-MAX_UINT32())-1 : $v;
}

##
## type: float
##
sub encode_float_le {
    $_[0] .= pack('f', $_[1]);
}
sub decode_float_le {
    die BROKEN_MESSAGE() if $_[1]+4 > length($_[0]); 
    my $v = unpack('f', substr($_[0], $_[1], 4));
    $_[1] += 4; 
    return $v;
}

sub encode_float_be {
    $_[0] .= reverse pack('f', $_[1]);
}
sub decode_float_be {
    die BROKEN_MESSAGE() if $_[1]+4 > length($_[0]); 
    my $v = unpack('f', reverse substr($_[0], $_[1], 4));
    $_[1] += 4; 
    return $v;
}


