package Google::ProtocolBuffers::CodeGen;
use strict;
use warnings;

use Google::ProtocolBuffers::Constants qw/:types :labels :complex_types/;

my %primitive_types = reverse (
    TYPE_DOUBLE => TYPE_DOUBLE,
    TYPE_FLOAT  => TYPE_FLOAT,
    TYPE_INT64  => TYPE_INT64,
    TYPE_UINT64 => TYPE_UINT64,
    TYPE_INT32  => TYPE_INT32,
    TYPE_FIXED64=> TYPE_FIXED64,
    TYPE_FIXED32=> TYPE_FIXED32,
    TYPE_BOOL   => TYPE_BOOL,
    TYPE_STRING => TYPE_STRING,
    TYPE_GROUP  => TYPE_GROUP,      ## 
    TYPE_MESSAGE=> TYPE_MESSAGE,    ## should never appear, because 'message' is a 'complex type'
    TYPE_BYTES  => TYPE_BYTES,
    TYPE_UINT32 => TYPE_UINT32,
    TYPE_ENUM   => TYPE_ENUM,       ## 
    TYPE_SFIXED32=>TYPE_SFIXED32,
    TYPE_SFIXED64=>TYPE_SFIXED64,
    TYPE_SINT32 => TYPE_SINT32,
    TYPE_SINT64 => TYPE_SINT64,
);

my %labels = reverse (
    LABEL_OPTIONAL  => LABEL_OPTIONAL,
    LABEL_REQUIRED  => LABEL_REQUIRED,
    LABEL_REPEATED  => LABEL_REPEATED,
);

sub _get_perl_literal {
    my $v = shift;
    my $opts = shift;
   
    if ($v =~ /^-?\d+$/) {
        ## integer literal
        if ($v>0x7fff_ffff || $v<-0x8000_0000) {
            return "Math::BigInt->new('$v')";
        } else {
            return "$v";
        }
     } elsif ($v =~ /[-+]?\d*\.\d+([Ee][\+-]?\d+)?|[-+]?\d+[Ee][\+-]?\d+/i) {        
        ## floating point literal
        return "$v";
    } else {
        ## string literal
        $v =~ s/([\x00-\x1f'"\\$@%\x80-\xff])/ '\\x{' . sprintf("%02x", ord($1)) . '}' /ge;
        return qq["$v"];
    } 
}

sub generate_code_of_enum {
    my $self = shift;
    my $opts = shift;
    
    my $class_name = ref($self) || $self;
    my $fields_text;
    foreach my $f (@{ $self->_pb_fields_list }) {
        my ($name, $value) = @$f;
        $value = _get_perl_literal($value, $opts); 
        $fields_text .= "               ['$name', $value],\n";
    }
    
    return <<"CODE";
    unless ($class_name->can('_pb_fields_list')) {
        Google::ProtocolBuffers->create_enum(
            '$class_name',
            [
$fields_text
            ]
        );
    }
    
CODE
}


sub generate_code_of_message_or_group {
    my $self = shift;
    my $opts = shift;

    my $create_what = 
        ($self->_pb_complex_type_kind==MESSAGE) ? 'create_message' :
        ($self->_pb_complex_type_kind==GROUP)   ? 'create_group'   : die;
                        
    my $class_name = ref($self) || $self;
    
    my $fields_text = ''; # may be empty, as empty messages are allowed
    foreach my $f (@{ $self->_pb_fields_list }) {
        my ($label, $type, $name, $field_number, $default_value) = @$f;
        
        die unless $labels{$label};
        $label = "Google::ProtocolBuffers::Constants::$labels{$label}()";

        if ($primitive_types{$type}) {
            $type = "Google::ProtocolBuffers::Constants::$primitive_types{$type}()";
        } else {
            $type = "'$type'";
        }
        
        $default_value = (defined $default_value) ? 
            _get_perl_literal($default_value, $opts) : 'undef'; 
        $fields_text .= <<"FIELD";             
                [
                    $label, 
                    $type, 
                    '$name', $field_number, $default_value
                ],
FIELD
    }

    my $oneofs_text = "            undef,\n";
    if ($self->can('_pb_oneofs')) {
        $oneofs_text = "            {\n";
        while (my ($name, $fields) = each %{$self->_pb_oneofs}) {
            $oneofs_text .= "                '$name' => [\n";
            foreach my $f (@$fields) {
                $oneofs_text .= "                    '$f',\n";
            }
            $oneofs_text .= "                ],\n";
        }
        $oneofs_text .= "            },\n";
    }

    my $options = '';
    foreach my $opt_name (qw/create_accessors follow_best_practice/) {
        if ($opts->{$opt_name}) {
            $options .= "'$opt_name' => 1, "    
        }
    }
    
    return <<"CODE";
    unless ($class_name->can('_pb_fields_list')) {
        Google::ProtocolBuffers->$create_what(
            '$class_name',
            [
$fields_text
            ],
$oneofs_text
            { $options }
        );
    }

CODE

}

1;
