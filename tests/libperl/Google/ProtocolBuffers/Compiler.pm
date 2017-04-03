package Google::ProtocolBuffers::Compiler;
use strict;
use warnings;
use Parse::RecDescent;
use Data::Dumper;
use Google::ProtocolBuffers::Constants qw/:types :labels/;
use Carp;
use Config qw/%Config/;
use File::Spec;

##
## Grammar is based on work by Alek Storm 
## http://groups.google.com/group/protobuf/browse_thread/thread/1cccfc624cd612da
## http://groups.google.com/group/protobuf/attach/33102cfc0c57d449/proto2.ebnf?part=4
## 

my $grammar = <<'END_OF_GRAMMAR';

proto       :   <skip: qr!  (?: //.*\n | \s+ )*  !x>
                ## list of top level declarations. 
                ## Skip empty declarations and ";".
                (message | extend | enum | import | package | option | service | syntax | ";")(s) /\Z/
                { $return = [ grep {ref $_} @{$item[2]} ]; }
            |   <error>

import      :   "import" <commit> strLit ";"
                { $return = [ import => $item{strLit} ]; }
                ## error? reject pair means: 
                ##  if rule was commited (i.e. "import" was found), then fail the entire parse
                ##  otherwise, just skip this production (and try another one)
            |   <error?> <reject>   

package     :   "package" <commit> qualifiedIdent ";"
                { $return = [ package => $item{qualifiedIdent} ]; }
            |   <error?> <reject>   

option      :   ## so far, options are ignored 
                "option" <commit> optionBody ";"
                { $return = '' }        
            |   <error?> <reject>

optionBody  :   qualifiedIdent "=" constant
                { $return = '' }

message     :   "message" <commit> ident messageBody
                { $return = [ message => $item{ident}, $item{messageBody} ]; }
            |   <error?> <reject>

extend      :   "extend" <commit> userType "{" ( field | group | ";" )(s?) "}"
                { $return = [extend => $item{userType}, [ grep {ref $_} @{$item[5]}] ]}
                
enum        :   "enum" <commit> ident "{" (option | enumField | ";")(s) "}"
                { $return = [ enum => $item{ident}, [grep {ref $_} @{$item[5]}] ] }
            |   <error?> <reject>

enumField   :   ident "=" intLit ";"
                { $return = [ enumField => $item{ident}, $item{intLit} ] }

service     :   ## services are ignored
                "service" <commit> ident "{" ( option | rpc | ";" )(s?) "}"
                { $return = '' }
            |   <error?> <reject>
            
rpc         :   "rpc" <commit> ident "(" userType ")" "returns" "(" userType ")" rpcOptions(?) ";"
                { $return = '' }
            |   <error?> <reject>

rpcOptions  :   "{" option(s?) "}"

messageBody :   "{" <commit> ( field | enum | message | extend | extensions | group | option | oneof | ";" )(s?) "}"
                { $return = [ grep {ref $_} @{$item[3]} ] }
            |   <error?> <reject>

group       :   label "group" <commit> ident "=" intLit messageBody
                { $return = [group => $item{label}, $item{ident}, $item{intLit}, $item{messageBody} ] }
            |   <error?> <reject>

field       :   label type ident "=" intLit fOptList(?) ";"
                { $return = [field => $item{label}, $item{type}, $item{ident}, $item{intLit}, $item[6][0] ] }

oneof       :   "oneof" <commit> ident "{" ( oneofField | ";" )(s?) "}"
                { $return = [ oneof => $item{ident}, [grep {ref $_} @{$item[5]}] ] }
            |   <error?> <reject>

oneofField  :   type ident "=" intLit fOptList(?) ";"
                { $return = [field => "optional", $item{type}, $item{ident}, $item{intLit}, $item[5][0] ] }

fOptList    :   "[" fieldOption(s? /,/) "]"
                { $return = (grep {length($_)} @{$item[2]})[0] || '' }

fieldOption :   "default" <commit> "=" constant
                { $return =  $item{constant} }
            |   optionBody 
                { $return = '' }
            |   <error?> <reject>
            
extensions  :   "extensions" <commit> extension(s /,/) ";"
                { $return = '' }
            |   <error?> <reject>

extension   :   intLit ( "to" ( intLit | "max" ) )(s?)
                { $return = '' }

label       :   "required" | "optional" | "repeated" 


type        :   "double" | "float" | "int32" | "int64" | "uint32" | "uint64"
            |   "sint32" | "sint64" | "fixed32" | "fixed64" | "sfixed32" | "sfixed64"
            |   "bool" | "string" | "bytes" | userType

userType    :   (".")(?) qualifiedIdent
                {  $return = ($item[1] && @{$item[1]}) ? ".$item[2]" : $item[2]  }

constant    :   ident 
                { $return = $item[1]; }
            |   (floatLit | intLit | strLit | boolLit)
                { $return = { value => $item[1] } }
            
ident       :   /[a-z_]\w*/i

qualifiedIdent:     
                <leftop: ident "." ident>
                { $return = join(".", @{ $item[1] })}

intLit      :   hexInt | octInt| decInt 

decInt      :   /[-+]?[1-9]\d*/
                { $return = Google::ProtocolBuffers::Compiler::get_dec_int($item[1]) }

hexInt      :   /[-+]?0[xX]([A-Fa-f0-9])+/
                { $return = Google::ProtocolBuffers::Compiler::get_hex_int($item[1]) }

octInt      :   /[-+]?0[0-7]*/
                { $return = Google::ProtocolBuffers::Compiler::get_oct_int($item[1]) }

floatLit    :   ## Make floatLit do not match integer literals,
                ## so that it doesn't take off '0' from '0xFFF' or '012' (oct).
                /[-+]?\d*\.\d+([Ee][\+-]?\d+)?/
            |   /[-+]?\d+[Ee][\+-]?\d+/
            

boolLit     :   "true"
                { $return = 1 } 
            |   "false"
                { $return = 0 }

strLit      :   /['"]/ <skip:''> ( hexEscape | octEscape | charEscape | regularChar)(s?) /['"]/
                { $return = join('', @{$item[3]}) }

regularChar :   ## all chars exept chr(0) and "\n"
                /[^\0\n'"]/ 

hexEscape   :   /\\[Xx]/ /[A-Fa-f0-9]{1,2}/
                { $return = chr(hex($item[2])) }

octEscape   :   '\\' /^0?[0-7]{1,3}/
                { $return = chr(oct("0$item[2]") & 0xFF); }

charEscape  :   /\\[abfnrtv\\'"]/
                { 
                    my $s = substr($item[1], 1, 1);
                    $return =   ($s eq 'a') ? "\a" :
                                ($s eq 'b') ? "\b" : 
                                ($s eq 'f') ? "\f" :
                                ($s eq 'n') ? "\n" :
                                ($s eq 'r') ? "\r" :
                                ($s eq 't') ? "\t" :
                                ($s eq 'v') ? "\x0b" : $s;
                }

                                 
syntax      :   "syntax" "=" strLit ## syntax = "proto2";
                { 
                    die "Unknown syntax" unless $item{strLit} eq 'proto2';
                    $return = ''; 
                }                 

END_OF_GRAMMAR

my %primitive_types = (
    "double"    => TYPE_DOUBLE,
    "float"     => TYPE_FLOAT,
    "int32"     => TYPE_INT32,
    "int64"     => TYPE_INT64,
    "uint32"    => TYPE_UINT32,
    "uint64"    => TYPE_UINT64,
    "sint32"    => TYPE_SINT32,
    "sint64"    => TYPE_SINT64,
    "fixed32"   => TYPE_FIXED32,
    "fixed64"   => TYPE_FIXED64,
    "sfixed32"  => TYPE_SFIXED32,
    "sfixed64"  => TYPE_SFIXED64,
    "bool"      => TYPE_BOOL,
    "string"    => TYPE_STRING,
    "bytes"     => TYPE_BYTES,
);

my %labels = (
    'required'  => LABEL_REQUIRED,
    'optional'  => LABEL_OPTIONAL,
    'repeated'  => LABEL_REPEATED,
);

my $has_64bit = $Config{ivsize}>=8;

sub _get_int_value {
    my $str = shift;
    my $max_pos_str = shift;
    my $max_neg_str = shift;
    my $str_to_num = shift;
    my $str_to_bigint = shift;

    my $is_negative = ($str =~/^-/);
    $str =~ s/^[+-]//;
    
    if (!$has_64bit) {
        my $l = length($str);
        if (
            !$is_negative &&
                ($l>length($max_pos_str) 
                || ($l==length($max_pos_str) && uc($str) ge uc($max_pos_str)))
            || $is_negative &&
                ( $l>length($max_neg_str) 
                || ($l==length($max_neg_str) && uc($str) ge uc($max_neg_str)))
        )
        {
            my $v = $str_to_bigint->($str); 
            return ($is_negative) ? -$v : $v;
        }
    }
    
    my $v = $str_to_num->($str);
    return ($is_negative) ? -$v : $v;
}

sub get_dec_int {
    my $str = shift;
    
    return _get_int_value(
        $str, "2147483647", "2147483648",
        sub {
            no warnings 'portable';
            return $_[0]+0;
        }, 
        sub {
            return Math::BigInt->new($_[0]);    
        }
    );
}
    
sub get_hex_int {
    my $str = shift;

    return _get_int_value(
        $str, "0x7fffffff", "0x80000000",
        sub {
            no warnings 'portable';
            return hex($_[0]);
        }, 
        sub {
            return Math::BigInt->new($_[0]);    
        }
    );
}

sub get_oct_int {
    my $str = shift;
    return _get_int_value(
        $str, "017777777777", "020000000000",
        sub {
            no warnings 'portable';
            return oct($_[0]);
        }, 
        sub {
            ## oops, Math::BigInt doesn't accept strings of octal digits,
            ## ... but accepts binary digits
            my $v = shift;
            my @oct_2_binary = qw(000 001 010 011 100 101 110 111);
            $v =~ s/(.)/$oct_2_binary[$1]/g;
            return Math::BigInt->new('0b' . $v);
        }
    );
}


sub parse {
    my $class = shift;
    my $source = shift;
    my $opts  = shift;

    my $self  = bless { opts => $opts }; 
    
    $::RD_ERRORS = 1;
    $::RD_WARN = 1;
    my $parser = Parse::RecDescent->new($grammar) or die;

    ## all top level declarations from all files (files be included)
    ## will be here
    my @parse_tree;

    my (@import_files, $text);
    if ($source->{text}) {
        $text = $source->{text};
    } elsif ($source->{file}) {
        @import_files = ('', $source->{file});
    } else {
        die;
    }

    my %already_included_files;

    while ($text || @import_files)  {
        my ($content, $filename);
        
        if ($text) {
            $content = $text;
            undef $text;
        } else {
            ## path may be relative to the path of the file, where
            ## "import" directive. Also, root dir for proto files
            ## may be specified in options
            my ($root, $path) = splice(@import_files, 0, 2);            
            $filename = $self->_find_filename($root, $path);
            next if $already_included_files{$filename}++;
            {
                my $fh;
                open $fh, $filename or die "Can't read from $filename: $!";
                local $/;
                $content = <$fh>;
                close $fh;
            }
        }
        
        my $res = $parser->proto($content);
        die "" unless defined $res;
        
        ## start each file from empty package
        push @parse_tree, [package=>''];
        foreach my $decl (@$res) {
            if ($decl->[0] eq 'import') {
                push @import_files, ($filename, $decl->[1]);
            } else {
                push @parse_tree, $decl;
            }
        }
    }
    
    ##
    ## Pass #1. 
    ## Find names of messages and enums, including nested ones.
    ## 
    my $symbol_table = Google::ProtocolBuffers::Compiler::SymbolTable->new;
    $self->{symbol_table} = $symbol_table;
    $self->collect_names('', \@parse_tree);

    ##
    ## Pass #2.
    ## Create complete descriptions of messages with extensions.
    ## For each field of a user type a fully quilified type name must be found.
    ## For each default value defined by a constant (enum), a f.q.n of enum value must be found
    ## 
    foreach my $kind (qw/message group enum oneof/) {
        foreach my $fqname ($symbol_table->lookup_names_of_kind($kind)) {
            $self->{types}->{$fqname} = { kind => $kind, fields => [], extensions => [], oneofs => [] };
        }
    }
    $self->collect_fields('', \@parse_tree);
    
    return $self->{types};
}

sub _find_filename {
    my $self = shift;
    my $base_filename = shift;
    my $path = shift;

=comment
    my $filename = File::Spec->rel2abs($path, $base_filename);
    return $filename if -e $filename;
    
    if ($self->{opts}->{include_dir}) {
        $filename = File::Spec->rel2abs($path, $self->{opts}->{include_dir});
        return $filename if -e $filename;
    }
=cut
    use Cwd; my $d = getcwd();
    
    my $filename = $path;
    return $filename if -e $filename;

    if (my $inc_dirs = $self->{opts}->{include_dir}) {
        $inc_dirs = [ $inc_dirs ] unless(ref($inc_dirs) eq 'ARRAY');
        foreach my $d (@$inc_dirs){
            $filename = File::Spec->catfile($d, $path);
            return $filename if -e $filename;
        }
    }
    die "Can't find proto file: '$path'";
}


sub collect_names {
    my $self = shift;
    my $context = shift;
    my $nodes = shift;
        
    my $symbol_table = $self->{symbol_table};
    foreach my $decl (@$nodes) {
        my $kind = $decl->[0]; ## 'message', 'extent', 'enum' etc...
        if ($kind eq 'package') {
            ## package directive just set new context, 
            ## not related to previous one
            $context = $symbol_table->set_package($decl->[1]);
        } elsif ($kind eq 'message') {
            ## message may include nested messages/enums/groups/oneofs
            my $child_context = $symbol_table->add('message' => $decl->[1], $context);
            $self->collect_names($child_context, $decl->[2]);
        } elsif ($kind eq 'enum') {
            my $child_context = $symbol_table->add('enum' => $decl->[1], $context);
            $self->collect_names($child_context, $decl->[2]);
        } elsif ($kind eq 'group') {
            ## there may be nested messages/enums/groups/oneofs etc. inside group
            ## [group => $label, $ident, $intLit, $messageBody ]            
            my $child_context = $symbol_table->add('group' => $decl->[2], $context);
            $self->collect_names($child_context, $decl->[4]);
        } elsif ($kind eq 'oneof') {
            ## OneOfs may only contain fields, we add them to both
            ## the current and oneof context
            my $child_context = $symbol_table->add('oneof' => $decl->[1], $context);
            foreach my $oneof (@{$decl->[2]}) {
                $symbol_table->add('field' => $oneof->[3], $context);
                $symbol_table->add('field' => $oneof->[3], $child_context);
            }
        } elsif ($kind eq 'extend') {
            ## extend blocks are tricky: 
            ## 1) they don't create a new scope
            ## 2) there may be a group inside extend block, and there may be everything inside the group 
            $self->collect_names($context, $decl->[2]);
        } elsif ($kind eq 'field') {
            ## we add fields into symbol table just to check their uniqueness 
            ## in several extension blocks or oneofs. Example:
            ##  .proto: 
            ##      extend A { required int32 foo = 100  };
            ##      extend B { required int32 foo = 200  }; 
            ##      // Invalid! foo is already declared!
            ##
            $symbol_table->add('field' => $decl->[3], $context);
        } elsif ($kind eq 'enumField') {
            $symbol_table->add('enum_field' => $decl->[1], $context);
        } else {
            warn $kind;
        }
    }
}

sub collect_fields {
    my $self = shift;
    my $context = shift;
    my $nodes = shift;
    my $destination_type_name = shift;
    my $is_extension = shift;
    
    my $symbol_table = $self->{symbol_table};
    foreach my $decl (@$nodes) {
        my $kind = $decl->[0]; ## 'message', 'extent', 'enum' etc...
        if ($kind eq 'package') {
            $context = $decl->[1];
        } elsif ($kind eq 'message') {
            my $child_context = ($context) ? "$context.$decl->[1]" : $decl->[1];
            $self->collect_fields($child_context, $decl->[2], $child_context);
        } elsif ($kind eq 'enum') {
            my $child_context = ($context) ? "$context.$decl->[1]" : $decl->[1];
            $self->collect_fields($child_context, $decl->[2], $child_context);
        } elsif ($kind eq 'group') {
            ## groups are tricky: they are both definition of a field and type.
            ## [group => $label, $ident, $intLit, $messageBody ]
            ## first, collect fields inside the group           
            my $child_context = ($context) ? "$context.$decl->[2]" : $decl->[2];
            $self->collect_fields($child_context, $decl->[4], $child_context);
            ## second, add the group as one field to parent (destination) type
            confess unless $destination_type_name;
            my $name;
            my $fields_list;
            if ($is_extension) {
                ## for extensions, fully quilified names of fields are used,
                ## because they may be declared anywhere - even in another package
                $fields_list = $self->{types}->{$destination_type_name}->{extensions}; 
                $name = $symbol_table->lookup('group' => $decl->[2], $context);
            } else {
                ## regualar fields are always immediate children of their type
                $fields_list = $self->{types}->{$destination_type_name}->{fields}; 
                $name = $decl->[2];
            }
            my $label = (exists $labels{$decl->[1]}) ? $labels{$decl->[1]} : die;
            my ($type_name, $kind) = $symbol_table->lookup_symbol($decl->[2], $context);
            die unless $kind eq 'group';
            my $field_number = $decl->[3];  
            push @$fields_list, [$label, $type_name, $name, $field_number];
        } elsif ($kind eq 'oneof') {
            my $child_context = ($context) ? "$context.$decl->[1]" : $decl->[1];
            $self->collect_fields($child_context, $decl->[2], $child_context);
            push @{$self->{types}->{$destination_type_name}->{oneofs}}, $child_context;
        } elsif ($kind eq 'extend') {
            ## what is the fqn of the message to be extended?
            my $destination_message = $symbol_table->lookup('message' => $decl->[1], $context);
            $self->collect_fields($context, $decl->[2], $destination_message, 1);
        } elsif ($kind eq 'field') {
            confess unless $destination_type_name;
            # $decl = ['field' => $label, $type, $ident, $item{intLit}, $item{fOptList}] }
            my $name;
            my $fields_list;
            if ($is_extension) {
                ## for extensions, fully quilified names of fields are used,
                ## because they may be declared anywhere - even in another package
                $fields_list = $self->{types}->{$destination_type_name}->{extensions}; 
                $name = $symbol_table->lookup('field' => $decl->[3], $context);
            } else {
                ## regualar fields are always immediate children of their type
                $fields_list = $self->{types}->{$destination_type_name}->{fields}; 
                $name = $decl->[3];
            }

            my $label = (exists $labels{$decl->[1]}) ? $labels{$decl->[1]} : die;

            my ($type_name, $kind);
            if (exists $primitive_types{$decl->[2]}) {
                $type_name = $primitive_types{$decl->[2]};
            } else {
                ($type_name, $kind) = $symbol_table->lookup_symbol($decl->[2], $context);
                die unless $kind eq 'message' || $kind eq 'group' || $kind eq 'enum';
            }

            my $field_number = $decl->[4];  

            my $default_value = $decl->[5];
            if ($default_value && !ref $default_value) {
            	if ($default_value eq 'true') {
            	   	$default_value = { value => 1 };
            	} elsif ($default_value eq 'false') {
            		$default_value = { value => 0 }; 
            	} else {
                    ## this default is enum value
                    ## type name must be fqn of enum type
                    die unless $kind eq 'enum';
                    $default_value = $symbol_table->lookup('enum_field' => $default_value, $type_name);
            	}
            }
            push @$fields_list, [$label, $type_name, $name, $field_number, $default_value];
        } elsif ($kind eq 'enumField') {
            confess unless $destination_type_name;
            my $fields_list = $self->{types}->{$destination_type_name}->{fields};
            push @{$fields_list}, [$decl->[1], $decl->[2]];
        } else {
            warn $kind;
        }
    }
}

package Google::ProtocolBuffers::Compiler::SymbolTable;
##
## %$self - symbol name table, descriptions of fully qualified names like Foo.Bar:
##  $names{'foo'}       = { kind => 'package' }
##  $names{'foo.Bar'}   = { kind => 'message' }
##  $names{'foo.Bar.Baz'}={ kind => 'enum',   }
##
use Data::Dumper;
use Carp;

sub new {
    my $class = shift;
    return bless {}, $class;
}

sub set_package {
    my $self = shift;
    my $package = shift;
    
    return '' unless $package;
    
    my @idents = split qr/\./, $package;
    my $name = shift @idents;
    while (1) {
        if (exists $self->{$name}) {
            die unless $self->{$name}->{kind} eq 'package';
        } else {
            $self->{$name} = {kind => 'package'}
        }
        last unless @idents;
        $name .= '.' . shift(@idents);
    }
    return $name;
}

sub _add {
    my $self = shift;
    my $kind = shift;
    my $name = shift;
    my $context = shift;
    
    ## no fully quilified names are alowed to declare (so far)
    die if $name =~ /\./;
    my $fqn;
    if ($context) {
        die "$name, $context" unless $self->{$context};
        $fqn = "$context.$name";
    } else {
        $fqn = $name;
    }

    if (exists $self->{$fqn}) {
        die "Name '$fqn' is already defined";
    } else {
        $self->{$fqn} = { kind=>$kind };
    }
    
    return $fqn;
}

sub add {
    my $self = shift;
    my $kind = shift;
    my $name = shift;
    my $context = shift;

    ## tricky: enum values are both children and siblings of enums
    if ($kind eq 'enum_field') {
        die unless $self->{$context}->{kind} eq 'enum';
        my $fqn = $self->_add($kind, $name, $context);
        $context =~ s/(^|\.)\w+$//; ## parent context
        $self->_add($kind, $name, $context);
        return $fqn;
    } else {
    	return $self->_add($kind, $name, $context);
    }
}

## input: fully or partially qualified name
## output: (fully qualified name, its kind - 'message', 'enum_field' etc.)
sub lookup_symbol {
    my $self = shift;
    my $n = shift;
    my $c = shift;
    
    my $context = $c;
    my $name = $n;
    if ($name =~ s/^\.//) {
        ## this is an fully quialified name
        if (exists $self->{$name}) {
            return ($name, $self->{$name}->{kind});
        }
    } else {
        ## relative name - look it up in the current context and up
        while (1) {
            my $fqn = ($context) ? "$context.$name" : $name;        
            if (exists $self->{$fqn}) {
                return ($fqn, $self->{$fqn}->{kind});
            }
            ## one level up
            last unless $context;
            $context =~ s/(^|\.)\w+$//; 
        }
    }
    die "Name '$name' ($c, $n) is not defined" . Data::Dumper::Dumper($self); 
}

## input: kind, fully or partially qualified name, context
## ouptut: fully qualified name
## if found kind of the name doesn't match given kind, an exception is raised
sub lookup {
    my $self = shift;
    my $kind = shift;
    my $name = shift;
    my $context = shift;
    
    my ($fqn, $k) = $self->lookup_symbol($name, $context);
    unless ($kind eq $k) {
    	confess "Error: while looking for '$kind' named '$name' in '$context', a '$k' named '$fqn' was found";
    }
    return $fqn;
}

## returns list of all fully qualified name of a given kind
sub lookup_names_of_kind {
    my $self = shift;
    my $kind = shift;
    
    return grep { $self->{$_}->{kind} eq $kind } keys %$self;   
}

1;
