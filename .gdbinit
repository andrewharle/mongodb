# Print the full stack trace on python exceptions to aid debugging
set python print-stack full

# Load the mongodb pretty printers
source buildscripts/gdb/mongo.py

# Load the mongodb lock analysis
source buildscripts/gdb/mongo_lock.py
