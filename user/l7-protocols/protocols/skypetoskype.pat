# Skype to Skype - UDP voice call (program to program) - http://skype.com
# Pattern attributes: ok veryfast overmatch
# Protocol groups: voip p2p proprietary
# Wiki: http://www.protocolinfo.org/wiki/Skype

# This matches at least some of the general chatter that occurs when the
# user isn't doing anything as well as actual calls.
# Thanks to Myles Uyema, mylesuyema AT gmail.com

skypetoskype
# require at least 16 bytes (my limited tests always get at least 18)
^..\x02.............

