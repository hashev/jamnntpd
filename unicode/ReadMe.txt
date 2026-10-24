Current C++23 server supports both Unicode reading and posting with Unicode-safe MIME. See README.md, docs/migration/text.md and examples/jamnntpd-utf8.xlat. The -noencode workaround below is not required for the new server.

Historical 1.x workaround documentation (retained for the shipped custom tables):

How to get JamNNTPd to display texts in Unicode
===============================================
With the help of the unicode/jamnntpd.xlat file and the *.chs files in 
unicode/xlat, it is possible to get JamNNTPd to display posts in Unicode. 
Posting in Unicode is unfortunately not possible. 

You should keep the "read" translations for iso-8859-1 in jamnntpd.xlat 
or JamNNTPd will not be able to properly translate the receiver name of 
messages that you post.

It is also necessary to run JamNNTPd with the -noencode switch since the 
MIME encoder in JamNNTPd is not Unicode-aware and may otherwise choose to 
split an utf-8 character into several MIME segments. This means that you
have to configure your newsreader to use utf-8 as the default charset. 

Sorry about that, this is a bit of a hack. :-)

