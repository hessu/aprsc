
Translating aprsc status view to other languages
===================================================

Create a new language file
----------------------------

Go to /opt/aprsc/web, and make a copy of the master English strings file. 
Look up your two-letter language code from
[the list](https://www.w3.org/International/articles/language-tags/)
('sv' is for Swedish, for example), and use it to construct the file name.

    cd /opt/aprsc/web
    cp strings-en.json strings-sv.json
    
Edit strings-sv.json with your favourite editor, translate all
the strings. Make sure you use UTF-8 encoding in your editor
and terminal.


Reconfigure or restart aprsc
-------------------------------

The aprsc process will scan for string files at startup and when
reloading configuration.  This step will only need to be done once
after creating the new string file - after further edits of strings,
simply reload the status web page.

To reload configuration, execute the `reload` option of the startup script.

On Ubuntu or Debian:

    sudo service aprsc reload

On Centos (and others):

    sudo /etc/init.d/aprsc reload


Add new language mapping
---------------------------

Edit /opt/aprsc/web/aprsc.js - in the beginning you'll find two
statements which need to be adjusted:

    // add additional language codes here, in the end of the list:
    var lang_list = ['en', 'fi'];
    // and, if necessary, add one new line in the beginning here,
    // for dialect mapping (en_US, en_GB to en):
    var lang_map = {
        'en_*': 'en',
        'fi_*': 'fi',
        '*': 'en' // DO NOT remove or change the default mapping to 'en'
    };

For Swedish translation, the first list would become:

    var lang_list = ['en', 'fi', 'sv'];

If Swedish would have dialects ('sv_FI' for Swedish spoken in Finland)
then the mapping for the variants can be added. Otherwise, only clients
requesting plain 'sv' for language will get the translation.

    var lang_map = {
        'en_*': 'en',
        'fi_*': 'fi',
        'sv_*': 'sv',
        '*': 'en'
    };

Pass new translation to upstream aprsc code
----------------------------------------------

Give your new translation back to the aprsc project, so that it will be
included in future versions automatically, and that it'll be installed
on other servers too.

