
Contributing to the aprsc project
====================================

aprsc is an open source project, so you're welcome to contribute bug fixes
and improvements.  However, before starting development of a new feature,
please post a message to the [aprsc discussion group][aprsc-group]
describing what you intend to do, and how you intend to do it, so that the
idea can be discussed a bit.

If your idea does not fit the original ideology of aprsc, it may well be that
your improvements will not be taken in the mainline aprsc source tree.  If
it seems to make sense, there are some requirements for source code style.

All patches need to come in as pull requests in github.  Fork the project,
make the changes, push the changes in your own fork, and make a pull request
to the main project.  If and when I have feedback to give about the patch,
I'll use the github tools to provide the feedback.  It's quite likely that
I'll ask you to make some small changes before pulling the changes in.

[aprsc-group]: https://groups.google.com/forum/#!forum/aprsc


Design guidelines
--------------------

1. Keep it small - don't bloat it with features.
2. Keep it fast and scalable.
3. It's an APRS-IS server - not an igate, not an object generator, not a
   database collector, and it will not brew coffee. Don't bloat it with
   features which belong to other software such as igates. Did I say that
   already?


Source code style
--------------------

1. Follow the original style used elsewhere in the source code.
2. Indent by 1 tab character (configure editor to display it as 8 spaces).
3. Don't reindent / re-format existing code while committing a fix
   somewhere - the patch / diff should highlight only the functional changes
   made in that commit.

The [Linux kernel coding style][linux-codestyle] document is a good read.

[linux-codestyle]: http://www.kernel.org/doc/Documentation/CodingStyle

I'm not saying the original style is the only good style, or the absolutely
right one.  I just require consistency within the project.  When I come
contributing to your project, I'll follow your style as well as I can.


Testing and quality control
------------------------------

All new feature commits _must_ come with a test case in the test suite.
It's the only way to keep that feature working in the future.

It's a good idea to start by getting the test suite running on your system
so that you will notice if your changes accidentally break some other
functionality.

Patches which simply add new test cases for existing, untested functionality
are more than welcome.  Writing tests in Perl is fun and an easy way to
start contributing.

Broken code on the live APRS-IS server network can break the whole network.
This has the potential of making a lot of people unhappy.  Before connecting
modified to the real APRS-IS, *DO* *RUN* the test suite to make sure it's
working to some degree.


Checking out current development source code
-----------------------------------------------

Go to the [aprsc repository on github](https://github.com/hessu/aprsc)
and do a clone.

Please learn to use git and github. They're good stuff and make things
easier for you and me.


License
----------

aprsc is licensed using the BSD license (found in src/LICENSE), and your
contributions need to be licensed the same way.

Please note that the BSD license allows code reuse in any way or form, in
amateur and non-amateur use, for commercial and non-commercial applications.
But that's OK.


Ideas that need some working on
----------------------------------

* Getting SCTP support fully functional
  * Start with client listeners, and client-side perl test cases, Matti
    will be happy to add support in the aprx igate I'm sure
* Getting on-line reconfiguration fully functional
  * Hessu is already working on that, pretty close to completion


Ideas that have been suggested, but will not be added
--------------------------------------------------------

* igating and other radio interfaces: These are igate functions, and
  should be implemented in igate software (aprsg, aprx, there are a lot
  of those - pick one and implement it there).
* beacon generation: Most if not all igate software can already beacon
  objects. No need to reimplement the functionality here.


