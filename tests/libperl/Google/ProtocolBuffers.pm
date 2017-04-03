package Google::ProtocolBuffers;

use 5.008008;
use warnings;
use strict;

use Google::ProtocolBuffers::Codec;
use Google::ProtocolBuffers::Constants qw/:complex_types :labels/;
use Class::Accessor;
use Math::BigInt;
use Carp;
use Data::Dumper;

our $VERSION = "0.12";

sub parsefile {
    my $self = shift;
    my $proto_filename = shift;
    my $opts = shift || {};
    
    return $self->_parse({file=>$proto_filename}, $opts);
}

sub parse {
    my $self = shift;
    my $proto_text = shift;
    my $opts = shift || {};

    return $self->_parse({text=>$proto_text}, $opts);
}

## Positional access is slightly faster than named one. 
## Currently, it's in the same order as text in proto file
## "optional" (LABEL) int32 (type) foo (name) = 1 (number) [default=...]
use constant {
    F_LABEL     => 0,
    F_TYPE      => 1,
    F_NAME      => 2,
    F_NUMBER    => 3,
    F_DEFAULT   => 4,       
};

sub _parse {
    my $self = shift;
    my $source = shift;
    my $opts = shift;

    require 'Google/ProtocolBuffers/Compiler.pm';
    my $types = Google::ProtocolBuffers::Compiler->parse($source, $opts);
   
    ## 
    ## 1. Create enums - they will be used as default values for fields
    ##
    my @created_enums;
    while (my ($type_name, $desc) = each %$types) {
        next unless $desc->{kind} eq 'enum';
        my $class_name = $self->_get_class_name_for($type_name, $opts);
        $self->create_enum($class_name, $desc->{fields});
        push @created_enums, $class_name;
    }
    
    ##
    ## 2. Create groups and messages, 
    ## Fill default values of fields and convert their 
    ## types (my_package.message_a) into Perl classes names (MyPackage::MessageA)
    ##
    my @created_messages;
    while (my ($type_name, $desc) = each %$types) {
        my $kind = $desc->{kind};
        my @fields;
        my %oneofs;
        
        if ($kind =~ /^(enum|oneof)$/) {
            next;
        } elsif ($kind eq 'group') {
            push @fields, @{$desc->{fields}};
        } elsif ($kind eq 'message') {
            push @fields, @{$desc->{fields}};

            ##
            ## Get names for extensions fields.
            ## Original (full quilified) name is like 'package.MessageA.field'.
            ## If 'simple_extensions' is true, it will be cut to the last element: 'field'.
            ## Otherwise, it will be enclosed in brackets and all part common to message type
            ## will be removed, e.g. for message 'package.MessageB' it will be '[MessageA.field]'
            ## If message is 'other_package.MessageB', it will be '[package.MessageA.field]'
            ##
            foreach my $e (@{$desc->{extensions}}) {
                my $field_name = $e->[F_NAME];
                my $new_name;   
                if ($opts->{simple_extensions}) {
                    $new_name = ($field_name =~ /\.(\w+)$/) ? $1 : $field_name; 
                } else {
                    ## remove common identifiers from start of f.q.i.
                    my @type_idents  = split qr/\./, $type_name;
                    my @field_idents = split qr/\./, $field_name;
                    while (@type_idents && @field_idents) {
                        last if $type_idents[0] ne $field_idents[0];
                        shift @type_idents;
                        shift @field_idents;
                    }
                    die "Can't create name for extension field '$field_name' in '$type_name'" 
                        unless @field_idents;
                    $new_name = '[' . join('.', @field_idents) . ']';
                }
                $e->[F_NAME] = $new_name;
                push @fields, $e;
            }   

            ##
            ## Get names for oneof fields.
            ##
            foreach my $oneof_name (@{$desc->{oneofs}}) {
                my $oneof = $types->{$oneof_name};
                my @oneof_fields = map { $_->[F_NAME] } @{$oneof->{fields}};
                my $new_name = ($oneof_name =~ /\.(\w+)$/) ? $1 : $oneof_name;
                $oneofs{$new_name} = \@oneof_fields;
                push @fields, @{$oneof->{fields}};
            }
        } else {
            die;
        } 
        
        ##
        ## Replace proto type names by Perl classes names
        ##
        foreach my $f (@fields) {
            my $type = $f->[F_TYPE];
            if ($type !~ /^\d+$/) {
                ## not a primitive type
                $f->[F_TYPE] = $self->_get_class_name_for($type, $opts);
            }
        }

        ##
        ## Default values: replace references to enum idents by their values
        ##
        foreach my $f (@fields) {
            my $default_value = $f->[F_DEFAULT];
            if ($default_value && ref $default_value) {
                ## this default value is a literal 
                die "Unknown default value " . Data::Dumper::Dumper($default_value) 
                    unless ref($default_value) eq 'HASH';
                $f->[F_DEFAULT] = $default_value->{value};
            } elsif ($default_value) {
                ## this default is an enum value
                my ($enum_name, $enum_field_name) = ($default_value =~ /(.*)\.(\w+)$/);
                my $class_name = $self->_get_class_name_for($enum_name, $opts);
                no strict 'refs';
                $f->[F_DEFAULT] = &{"${class_name}::$enum_field_name"};
                use strict;
            }
        }
        
        ##
        ## Create Perl classes
        ##
        my $class_name = $self->_get_class_name_for($type_name, $opts);
        if ($kind eq 'message') {
            $self->create_message($class_name, \@fields, \%oneofs, $opts);
        } elsif ($kind eq 'group') {
            $self->create_group($class_name, \@fields, $opts);
        }
        push @created_messages, $class_name;
    }

    my @created_classes = sort @created_enums;
    push @created_classes, sort @created_messages;

    ## Generate Perl code of created classes
    if ($opts->{generate_code}) {
        require 'Google/ProtocolBuffers/CodeGen.pm';
        my $fh;
        if (!ref($opts->{generate_code})) {
            open($fh, ">$opts->{generate_code}") 
                or die "Can't write to '$opts->{generate_code}': $!";
        } else {
            $fh = $opts->{generate_code};
        }
        
        my $package_str = ($opts->{'package_name'}) ?
            "package $opts->{'package_name'};" : "";

        my $source_str = ($source->{'file'}) ?
            "$source->{'file'}" : "inline text";

        print $fh <<"HEADER";
# Generated by the protocol buffer compiler (protoc-perl) DO NOT EDIT!
# source: $source_str

$package_str

use strict;
use warnings;

use Google::ProtocolBuffers;
{
HEADER
        foreach my $class_name (@created_classes) {
            print $fh $class_name->getPerlCode($opts);
        }
        print $fh "}\n1;\n";
    }
    return @created_classes;
}

# Google::ProtocolBuffers->create_message(
#  'AccountRecord',
#  [
#      ## required      string        name  = 1;
#      [LABEL_REQUIRED, TYPE_STRING,  'name', 1 ],
#      [LABEL_OPTIONAL, TYPE_INT32,   'id',   2 ],
#  ],
# );
sub create_message {
    my $self = shift;
    my $class_name = shift;
    my $fields = shift;
    my $oneofs = shift;
    my $opts = shift;
    
    return $self->_create_message_or_group(
        $class_name, $fields, $oneofs, $opts,
        'Google::ProtocolBuffers::Message'   
    );  
}

sub create_group {
    my $self = shift;
    my $class_name = shift;
    my $fields = shift;
    my $opts = shift;
    
    return $self->_create_message_or_group(
        $class_name, $fields, undef, $opts,
        'Google::ProtocolBuffers::Group'   
    );  
}
    
sub _create_message_or_group {
    my $self = shift;
    my $class_name = shift;
    my $fields = shift;
    my $oneofs = shift;
    my $opts = shift;
    my $base_class = shift;
    
    ##
    ## Sanity checks
    ##  1. Class name must be a valid Perl class name 
    ##  (should we check that this class doesn't exist yet?)
    ##
    die "Invalid class name: '$class_name'" 
        unless $class_name =~ /^[a-z_]\w*(?:::[a-z_]\w*)*$/i;
        
    ##
    ## 
    my (%field_names, %field_numbers);
    foreach my $f (@$fields) {
        my ($label, $type_name, $name, $field_number, $default_value) = @$f;
        die Dumper $f unless $name;
        
        ##
        ## field names must be valid identifiers and be unique
        ##
        die "Invalid field name: '$name'" 
            unless $name && $name =~ /^\[?[a-z_][\w\.]*\]?$/i;
        if ($field_names{$name}++) {
            die "Field '$name' is defined more than once";
        }
    
        ##
        ## field number must be positive and unique
        ##
        die "Invalid field number: $field_number" unless $field_number>0;
        if ($field_numbers{$field_number}++) {
            die "Field number $field_number is used more than once";
        } 
            
        ## type is either a number (for primitive types)
        ## or a class name. Can't check that complex $type 
        ## is valid, because it may not exist yet.
        die "Field '$name' doesn't has a type" unless $type_name;
        if ($type_name =~/^\d+$/) {
            ## ok, this is an ID of primitive type
        } else {
            die "Type '$type_name' is not valid Perl class name" 
                unless $type_name =~ /^[a-z_]\w*(?:::[a-z_]\w*)*$/i;
        }
        
        die "Unknown label value: $label" 
            unless $label==LABEL_OPTIONAL || $label==LABEL_REQUIRED || $label==LABEL_REPEATED; 
    }
    
    
    ## Make a copy of values and sort them so that field_numbers increase,
    ## this is a requirement of protocol
    ## Postitional addressation of field parts is sucks, TODO: replace by hash
    my @field_list               = sort { $a->[F_NUMBER] <=> $b->[F_NUMBER] } map { [@$_] } @$fields;
    my %fields_by_field_name     = map { $_->[F_NAME]   => $_ } @field_list;
    my %fields_by_field_number   = map { $_->[F_NUMBER] => $_ } @field_list;
    
    my $has_oneofs = defined($oneofs) && %$oneofs;
    my %oneofs_rev;

    if ($has_oneofs) {
        while (my ($name, $fields) = each %$oneofs) {
            %oneofs_rev = (%oneofs_rev, map { $_, $name } @$fields);
        }
    }

    no strict 'refs';
    @{"${class_name}::ISA"} = $base_class;
    *{"${class_name}::_pb_fields_list"}         = sub { \@field_list              };
    *{"${class_name}::_pb_fields_by_name"}      = sub { \%fields_by_field_name    };
    *{"${class_name}::_pb_fields_by_number"}    = sub { \%fields_by_field_number  };
    if ($has_oneofs) {
        *{"${class_name}::_pb_oneofs"}          = sub { $oneofs                   };
        *{"${class_name}::_pb_oneofs_rev"}      = sub { \%oneofs_rev              };
    }
    use strict;
    
    if ($opts->{create_accessors}) {
        no strict 'refs';
        push @{"${class_name}::ISA"}, 'Class::Accessor';
        if ($has_oneofs) {
            *{"${class_name}::new"} = \&Google::ProtocolBuffers::new;
            *{"${class_name}::which_oneof"} = \&Google::ProtocolBuffers::which_oneof;
        }
        *{"${class_name}::get"} = \&Google::ProtocolBuffers::get;
        *{"${class_name}::set"} = \&Google::ProtocolBuffers::set;
        use strict;

        if ($opts->{follow_best_practice}) {
            $class_name->follow_best_practice;
        }
        my @accessors = grep { /^[a-z_]\w*$/i } map { $_->[2] } @$fields;
        $class_name->mk_accessors(@accessors);
    }
}

sub create_enum {
    my $self = shift;
    my $class_name = shift;
    my $fields = shift;
    my $options = shift;

    ##
    ## Sanity checks
    ##  1. Class name must be a valid Perl class name 
    ##  (should we check that this class doesn't exist yet?)
    ##  2. Field names must be valid identifiers and be unique
    ##
    die "Invalid class name: '$class_name'" 
        unless $class_name =~ /^[a-z_]\w*(?:::[a-z_]\w*)*$/i;
    my %names;
    foreach my $f (@$fields) {
        my ($name, $value) = @$f;
        die "Invalid field name: '$name'" 
            unless $name && $name =~ /^[a-z_]\w*$/i;
        if ($names{$name}++) {
            die "Field '$name' is defined more than once";
        }
    }
    
    ## base class and constants export
    no strict 'refs'; 
    @{"${class_name}::ISA"} = "Google::ProtocolBuffers::Enum";
    %{"${class_name}::EXPORT_TAGS"} = ('constants'=>[]); 
    use strict;
    
    ## create the constants
    foreach my $f (@$fields) {
        my ($name, $value) = @$f;
        no strict 'refs';
        *{"${class_name}::$name"}   = sub { $value };
        push @{ ${"${class_name}::EXPORT_TAGS"}{'constants'} }, $name;
        push @{"${class_name}::EXPORT_OK"}, $name;
        use strict;     
    }
    
    ## create a copy of fields for introspection/code generation
    my @fields = map { [@$_] } @$fields;
    no strict 'refs';
    *{"${class_name}::_pb_fields_list"} = sub { \@fields };
    
}

##
## Accessors
##
sub getExtension {
    my $self = shift;
    my $data = (ref $self) ? $self : shift();
    my $extension_name = shift;
    
    unless($extension_name){
        return \%{$self->_pb_fields_by_name()};
    }
    
    $extension_name =~ s/::/./g;
    my $key = "[$extension_name]";
    
    my $field = $self->_pb_fields_by_name->{$key};
    if ($field) {
        return (exists $data->{$key}) ? $data->{$key} : $field->[F_DEFAULT];
    } else {
        my $class_name = ref $self || $self;
        die "There is no extension '$extension_name' in '$class_name'";
    }
}
    
    

sub setExtension {
    my $self = shift;
    my $data = (ref $self) ? $self : shift();
    my $extension_name = shift;
    my $value = shift;
    
    $extension_name =~ s/::/./g;
    my $key = "[$extension_name]";

    if ($self->_pb_fields_by_name->{$key}) {
        $data->{$key} = $value;
    } else {
        my $class_name = ref $self || $self;
        die "There is no extension '$extension_name' in '$class_name'";
    }
}

##
## Overide the Class::Accessor new to handle oneof fields.
##
sub new {
    my ($proto, $fields) = @_;
    my ($class) = ref $proto || $proto;

    $fields = {} unless defined $fields;

    my $self = bless {}, $class;

    ## Set the fields
    while (my ($key, $value) = each %$fields) {
        if (!defined($value)) {
            $self->{$key} = undef;
        }
        else {
            $self->set($key, $value);
        }
    }

    return $self;
}

##
## Return which field in a oneof is set
##
sub which_oneof {
    my $self = shift;
    my $oneof = shift;

    return undef unless $self->can('_pb_oneofs') &&
                        exists($self->_pb_oneofs->{$oneof});

    foreach my $f (@{$self->_pb_oneofs->{$oneof}}) {
        if (defined($self->{$f})) {
            return $f;
        }
    }

    return undef;
}

##
## This is for Class::Accessor read-accessors, will be
## copied to classes from Message/Group.
## If no value is set, the default one will be returned.
##
sub get {
    my $self = shift;

    if (@_==1) {
    	## checking that $self->{$_[0]} exists is not enough,
    	## since undef value may be set via Class::Accessor's new, e.g:
    	## my $data = My::Message->new({ name => undef })
        return $self->{$_[0]} if defined $self->{$_[0]};
        my $field = $self->_pb_fields_by_name->{$_[0]};
        return $field->[F_DEFAULT];
    } elsif (@_>1) {
    	my @rv;
    	my $fields;
    	foreach my $key (@_) {
    		if (defined $self->{$key}) {
    			push @rv, $self->{$key};
    		} else {
    			$fields ||= $self->_pb_fields_by_name;
    			push @rv, $fields->{$key}->[F_DEFAULT]; 
    		}
    	}
        return @rv;
    } else {
        Carp::confess("Wrong number of arguments received.");
    }
}

sub set {
    my $self = shift;
    my $key = shift;

    if (@_==1) {
    	if (defined $_[0]) {
    	   $self->{$key} = $_[0]; 	
    	} else {
    		delete $self->{$key};
    	}
    } elsif (@_>1) {
        $self->{$key} = [@_];   
    } else {
        Carp::confess("Wrong number of arguments received.");
    }

    # Is this a oneof field
    if ($self->can('_pb_oneofs_rev') && exists($self->_pb_oneofs_rev->{$key})) {
        foreach my $f (@{$self->_pb_oneofs->{$self->_pb_oneofs_rev->{$key}}}) {
            delete $self->{$f} unless $f eq $key;
        }
    }
}

sub _get_class_name_for{
    my $self = shift;
    my $type_name = shift;
    my $opts = shift;
    
    if ($opts->{no_camel_case}) {
        my $class_name = $type_name;
        $class_name  =~ s/\./::/g;
        return $class_name;
    } else {
        my @idents = split qr/\./, $type_name;
        foreach (@idents) {
            s/_(.)/uc($1)/ge;
            $_ = "\u$_";
        }
        return join("::", @idents);
    }       
}

package Google::ProtocolBuffers::Message;
no warnings 'once';
## public
*encode                 = \&Google::ProtocolBuffers::Codec::encode;
*decode                 = \&Google::ProtocolBuffers::Codec::decode;
*setExtension           = \&Google::ProtocolBuffers::setExtension;
*getExtension           = \&Google::ProtocolBuffers::getExtension;
*getPerlCode            = \&Google::ProtocolBuffers::CodeGen::generate_code_of_message_or_group;
## internal
##  _pb_complex_type_kind can be removed and $class->isa('Google::ProtocolBuffers::Message')
##  can be used instead, but current implementation is faster
sub _pb_complex_type_kind { Google::ProtocolBuffers::Constants::MESSAGE() } 
#   _pb_fields_list        ## These 3 methods are created in 
#   _pb_fields_by_name     ## namespace of derived class
#   _pb_fields_by_number

package Google::ProtocolBuffers::Group;
*setExtension           = \&Google::ProtocolBuffers::setExtension;
*getExtension           = \&Google::ProtocolBuffers::getExtension;
*getPerlCode            = \&Google::ProtocolBuffers::CodeGen::generate_code_of_message_or_group;
sub _pb_complex_type_kind { Google::ProtocolBuffers::Constants::GROUP() } 
#_pb_fields_list        
#_pb_fields_by_name
#_pb_fields_by_number  

package Google::ProtocolBuffers::Enum;
use base 'Exporter';
*getPerlCode            = \&Google::ProtocolBuffers::CodeGen::generate_code_of_enum;
sub _pb_complex_type_kind { Google::ProtocolBuffers::Constants::ENUM() } 
#_pb_fields_list        

1;

__END__

=pod

=head1 NAME

Google::ProtocolBuffers - simple interface to Google Protocol Buffers

=head1 SYNOPSYS

    ##
    ## Define structure of your data and create serializer classes
    ##
    use Google::ProtocolBuffers;
    Google::ProtocolBuffers->parse("
        message Person {
          required string name  = 1;
          required int32 id     = 2; // Unique ID number for this person.
          optional string email = 3;
        
          enum PhoneType {
            MOBILE = 0;
            HOME = 1;
            WORK = 2;
          }
        
          message PhoneNumber {
            required string number = 1;
            optional PhoneType type = 2 [default = HOME];
          }
        
          repeated PhoneNumber phone = 4;
        }
    ",
        {create_accessors => 1 }
    );
    
    ##
    ## Serialize Perl structure and print it to file
    ##
    open my($fh), ">person.dat";
    binmode $fh;
    print $fh Person->encode({
        name    => 'A.U. Thor',
        id      => 123,
        phone   => [ 
            { number => 1234567890 }, 
            { number => 987654321, type=>Person::PhoneType::WORK() }, 
        ],
    });
    close $fh;
    
    ##
    ## Decode data from serialized form
    ##
    my $person;
    {
        open my($fh), "<person.dat";
        binmode $fh;
        local $/;
        $person = Person->decode(<$fh>);
        close $fh;
    }
    print $person->{name}, "\n";
    print $person->name,   "\n";  ## ditto

=head1 DESCRIPTION

Google Protocol Buffers is a data serialization format. 
It is binary (and hence compact and fast for serialization) and as extendable
as XML; its nearest analogues are Thrift and ASN.1.
There are official mappings for C++, Java and Python languages; this library is a mapping for Perl. 

=head1 METHODS

=head2 Google::ProtocolBuffers->parse($proto_text, \%options)

=head2 Google::ProtocolBuffers->parsefile($proto_filename, \%options)

Protocol Buffers is a typed protocol, so work with it starts with some kind
of Interface Definition Language named 'proto'. 
For the description of the language, please see the official page
(L<http://code.google.com/p/protobuf/>)
Methods 'parse' and 'parsefile' take the description of data structure
as text literal or as name of the proto file correspondently.
After successful compilation, Perl serializer classes are created for each
message, group or enum found in proto. In case of error, these methods will 
die. On success, a list of names of created classes is returned.
Options are given as a hash reference, the recognizable options are: 

=over 4

=item include_dir => [ $dir_name ]

One proto file may include others, this option sets where to look for the
included files. Multiple dirs should be specificed as an ARRAYREF.

=item generate_code => $filename or $file_handler

Compilation of proto source is a relatively slow and memory consuming 
operation, it is not recommended in production environment. Instead, 
with this option you may specify filename or filehandle where to save
Perl code of created serializer classes for future use. Example:

    ## in helper script
    use Google::ProtocolBuffers;
    Google::ProtocolBuffers->parse(
        "message Foo {optional int32 a = 1; }",
        { generate_code => 'Foo.pm' }
    );
    
    ## then, in production code
    use Foo;
    my $str = Foo->encode({a => 100});

=item create_accessors (Boolean)

If this option is set, then result of 'decode' will be a blessed structure 
with accessor methods for each field, look at L<Class::Accessor> for more info.
Example:

    use Google::ProtocolBuffers;
    Google::ProtocolBuffers->parse(
        "message Foo { optional int32 id = 1; }",
        { create_accessors => 1 }
    );
    my $foo = Foo->decode("\x{08}\x{02}");
    print $foo->id; ## prints 2
    $foo->id(100);  ## now it is set to 100

=item follow_best_practice (Boolean)

This option is from L<Class::Accessor> too; it has no effect without 
'create_accessors'. If set, names of getters (read accessors) will 
start with get_ and names of setter with set_:

    use Google::ProtocolBuffers;
    Google::ProtocolBuffers->parse(
        "message Foo { optional int32 id = 1; }",
        { create_accessors => 1, follow_best_practice => 1 }
    );
    ## Class::Accessor provides a constructor too
    my $foo = Foo->new({ id => 2 }); 
    print $foo->get_id;  
    $foo->set_id(100);     

=item simple_extensions (Boolean)

If this option is set, then extensions are treated as if they were 
regular fields in messages or groups:

    use Google::ProtocolBuffers;
    use Data::Dumper;
    Google::ProtocolBuffers->parse(
        "   
            message Foo { 
                optional int32 id = 1;
                extensions 10 to max;     
            }
            extend Foo {
               optional string name = 10;
            }
        ",
        { simple_extensions=>1, create_accessors => 1 }
    );
    my $foo = Foo->decode("\x{08}\x{02}R\x{03}Bob");
    print Dumper $foo; ## { id => 2, name => 'Bob' }
    print $foo->id, "\n";
    $foo->name("Sponge Bob");

This option is off by default because extensions live in a separate namespace
and may have the same names as fields. Compilation of such proto with 
'simple_extension' option will result in die.
If the option is off, you have to use special accessors for extension fields - 
setExtension and getExtension, as in C++ Protocol Buffer API. Hash keys for 
extended fields in Plain Old Data structures will be enclosed in brackets:

    use Google::ProtocolBuffers;
    use Data::Dumper;
    Google::ProtocolBuffers->parse(
        "   
            message Foo { 
                optional int32 id = 1;
                extensions 10 to max;     
            }
            extend Foo {
               optional string id = 10; // <-- id again!
            }
        ",
        {   simple_extensions   => 0,   ## <-- no simple extensions 
            create_accessors    => 1, 
        }
    );
    my $foo = Foo->decode("\x{08}\x{02}R\x{05}Kenny");
    print Dumper $foo;      ## { id => 2, '[id]' => 'Kenny' }
    print $foo->id, "\n";                   ## 2
    print $foo->getExtension('id'), "\n";   ## Kenny
    $foo->setExtension("id", 'Kenny McCormick');

=item no_camel_case (Boolean)

By default, names of created Perl classes are taken from 
"camel-cased" names of proto's packages, messages, groups and enums.
First characters are capitalized, all underscores are removed and 
the characters following them are capitalized too. An example: 
a fully qualified name 'package_test.Message' will result in Perl class
'PackageTest::Message'. Option 'no_camel_case' turns name-mangling off.
Names of fields, extensions and enum constants are not affected anyway.

=item package_name (String)

Package name to be put into generated Perl code; has no effect on Perl classes names and
has no effect unless 'generate_code' is also set.

=back

=head2 MessageClass->encode($hashref)

This method may be called as class or instance method. 'MessageClass' must
already be created by compiler. Input is a hash reference.
Output is a scalar (string) with serialized data. 
Unknown fields in hashref are ignored. 
In case of errors (e.g. required field is not set and there is no default value
for the required field) an exception is thrown. 
Examples:

    use Google::ProtocolBuffers;
    Google::ProtocolBuffers->parse(
        "message Foo {optional int32 id = 1; }",
        {create_accessors => 1}
    );
    my $string = Foo->encode({ id => 2 });
    my $foo = Foo->new({ id => 2 });
    $string = $foo->encode;                 ## ditto
    
=head2 MessageClass->decode($scalar)

Class method. Input: serialized data string. Output: data object of class
'MessageClass'. Unknown fields in serialized data are ignored.
In case of errors (e.g. message is broken or partial) or data string is
a wide-character (utf-8) string, an exception is thrown.

=head1 PROTO ELEMENTS

=head2 Enums

For each enum in proto, a Perl class will be constructed with constants for
each enum value. You may import these constants via 
ClassName->import(":constants") call. Please note that Perl compiler 
will know nothing about these constants at compile time, because this import
occurs at run time, so parenthesis after constant's name are required.

    use Google::ProtocolBuffers;
    Google::ProtocolBuffers->parse(
        "
            enum Foo {
        	   FOO = 1;
        	   BAR = 2; 
            }
        ", 
        { generate_code => 'Foo.pm' }
    ); 
    print Foo::FOO(), "\n";     ## fully quailified name is fine
    Foo->import(":constants");
    print FOO(), "\n";          ## now FOO is defined in our namespace
    print FOO;                  ## <-- Error! FOO is bareword!

Or, do the import inside a BEGIN block:

    use Foo;                    ## Foo.pm was generated in previous example
    BEGIN { Foo->import(":constants") }
    print FOO, "\n";            ## ok, Perl compiler knows about FOO here

=head2 Groups

Though group are considered deprecated they are supported by Google::ProtocolBuffers.
They are like nested messages, except that nested type definition and field
definition go together:

    use Google::ProtocolBuffers;
    Google::ProtocolBuffers->parse(
        "
            message Foo {
            	optional group Bar = 1 {
                    optional int32 baz = 1;
            	}
            }
        ",
        { create_accessors => 1 }
    );
    my $foo = Foo->new;
    $foo->Bar( Foo::Bar->new({ baz => 2 }) );
    print $foo->Bar->baz, ", ", $foo->{Bar}->{baz}, "\n";   # 2, 2 


=head2 Default values

Proto file may specify a default value for a field. 
The default value is returned by accessor if there is no value for field
or if this value is undefined. The default value is not accessible via 
plain old data hash, though. Default string values are always byte-strings,
if you need wide-character (Unicode) string, use L<Encode/decode_utf8>.

    use Google::ProtocolBuffers;
    Google::ProtocolBuffers->parse(
        "message Foo {optional string name=1 [default='Kenny'];} ",
        {create_accessors => 1}
    );
    
    ## no initial value
    my $foo = Foo->new; 
    print $foo->name(), ", ", $foo->{name}, "\n"; # Kenny, (undef)   
    
    ## some defined value        
    $foo->name('Ken');           
    print $foo->name(), ", ", $foo->{name}, "\n"; # Ken, Ken   
    
    ## empty, but still defined value    
    $foo->name('');   
    print $foo->name(), ", ", $foo->{name}, "\n"; # (empty), (empty)  
    
    ## undef value == default value 
    $foo->name(undef);
    print $foo->name(), ", ", $foo->{name}, "\n"; # Kenny, (undef)   

=head2 Extensions

From the point of view of serialized data, there is no difference if a
field is declared as regular field or if it is extension, as far
as field number is the same.
That is why there is an option 'simple_extensions' (see above) that treats extensions
like regular fields.
From the point of view of named accessors, however, extensions live in 
namespace different from namespace of fields, that's why they simple names
(i.e. not fully qualified ones) may conflict. 
(And that's why this option is off by default).
The name of extensions are obtained from their fully qualified names from 
which leading part, most common with the class name to be extended, 
is stripped. Names of hash keys enclosed in brackets; 
arguments to methods 'getExtension' and 'setExtension' do not.
Here is the self-explanatory example to the rules:

    use Google::ProtocolBuffers;
    use Data::Dumper;
    
    Google::ProtocolBuffers->parse(
        "
            package some_package;
            // message Plugh contains one regular field and three extensions
            message Plugh {
            	optional int32 foo = 1;
                extensions 10 to max;
            }
            extend Plugh {
            	optional int32 bar = 10;
            }
            message Thud {
                extend Plugh {
                    optional int32 baz = 11;
                }
            }
            
            // Note: the official Google's proto compiler does not allow 
            // several package declarations in a file (as of version 2.0.1).
            // To compile this example with the official protoc, put lines
            // above to some other file, and import that file here.
            package another_package;
            // import 'other_file.proto';
            
            extend some_package.Plugh {
            	optional int32 qux = 12;
            }
            
        ",
        { create_accessors => 1 }
    );
    
    my $plugh = SomePackage::Plugh->decode(
        "\x{08}\x{01}\x{50}\x{02}\x{58}\x{03}\x{60}\x{04}"
    );
    print Dumper $plugh; 
    ## {foo=>1, '[bar]'=>2, '[Thud.baz]'=>3, [another_package.qux]=>4}
    
    print $plugh->foo, "\n";                            ## 1
    print $plugh->getExtension('bar'), "\n";            ## 2
    print $plugh->getExtension('Thud.baz'), "\n";       ## 3
    print $plugh->getExtension('Thud::baz'), "\n";      ## ditto

Another point is that 'extend' block doesn't create new namespace
or scope, so the following proto declaration is invalid:

    // proto:
    package test;
    message Foo { extensions 10 to max; } 
    message Bar { extensions 10 to max; }
    extend Foo { optional int32 a = 10; }
    extend Bar { optional int32 a = 20; }   // <-- Error: name 'a' in package
                                            // 'test' is already used! 

Well, extensions are the most complicated part of proto syntax, and I hope 
that you either got it or you don't need it.

=head1 RUN-TIME MESSAGE CREATION

You don't like to mess with proto files? 
Structure of your data is known at run-time only?
No problem, create your serializer classes at run-time too with method
Google::ProtocolBuffers->create_message('ClassName', \@fields, \%options);
(Note: The order of field description parts is the same as in 
proto file. The API is going to change to accept named parameters, but
backward compatibility will be preserved).

    use Google::ProtocolBuffers;
    use Google::ProtocolBuffers::Constants(qw/:labels :types/);
    
    ##
    ## proto:
    ## message Foo {
    ##      message Bar {
    ##	         optional int32 a = 1 [default=12];
    ##      }
    ##      required int32 id = 1;
    ##      repeated Bar   bars = 2;	
    ## }
    ##
    Google::ProtocolBuffers->create_message(
        'Foo::Bar',
        [
            ## optional      int32        a = 1 [default=12]
            [LABEL_OPTIONAL, TYPE_INT32, 'a', 1, '12']
        ],
        { create_accessors => 1 }
    );
    Google::ProtocolBuffers->create_message(
        'Foo',
        [
            [LABEL_REQUIRED, TYPE_INT32, 'id',   1],
            [LABEL_REPEATED, 'Foo::Bar', 'bars', 2],
        ],
        { create_accessors => 1 }
    );
    my $foo = Foo->new({ id => 10 });
    $foo->bars( Foo::Bar->new({a=>1}), Foo::Bar->new({a=>2}) );
    print $foo->encode;

There are methods 'create_group' and 'create_enum' also; the following constants 
are exported: labels 
(LABEL_OPTIONAL, LABEL_OPTIONAL, LABEL_REPEATED) 
and types
(TYPE_INT32, TYPE_UINT32, TYPE_SINT32, TYPE_FIXED32, TYPE_SFIXED32,
TYPE_INT64, TYPE_UINT64, TYPE_SINT64, TYPE_FIXED64, TYPE_SFIXED64, 
TYPE_BOOL, TYPE_STRING, TYPE_BYTES, TYPE_DOUBLE, TYPE_FLOAT).

=head1 KNOWN BUGS, LIMITATIONS AND TODOs

All proto options are ignored except default values for fields; 
extension numbers are not checked. 
Unknown fields in serialized data are skipped, 
no stream API (encoding to/decoding from file handlers) is present. 
Ask for what you need most.

Introspection API is planned.

Declarations of RPC services are currently ignored, but their support
is planned (btw, which Perl RPC implementation would you recommend?)

=head1 SEE ALSO

Official page of Google's Protocol Buffers project 
(L<http://code.google.com/p/protobuf/>)

Protobuf-PerlXS project (L<http://code.google.com/p/protobuf-perlxs/>) - 
creates XS wrapper for C++ classes generated by official Google's
compiler protoc. You have to complile XS files every time you've
changed the proto description, however, this is the fastest way to work 
with Protocol Buffers from Perl.

Protobuf-Perl project L<http://code.google.com/p/protobuf-perl/> - 
someday it may be part of official Google's compiler.

Thrift L<http://developers.facebook.com/thrift/>

ASN.1 L<http://en.wikipedia.org/wiki/ASN.1>, 
L<JSON> and L<YAML>

=head1 AUTHOR, ACKNOWLEDGEMENS, COPYRIGHT

Author: Igor Gariev <gariev@hotmail.com>
        the CSIRT Gadgets Foundation <csirtgadgets.org>

Proto grammar is based on work by Alek Storm
L<http://groups.google.com/group/protobuf/browse_thread/thread/1cccfc624cd612da>

This library is free software; you can redistribute it and/or modify
it under the same terms as Perl itself, either Perl version 5.10.0 or,
at your option, any later version of Perl 5 you may have available.
