
import os

def getFiles():

    root = "third_party/pcre-7.4"

    def pcreFilter(x):
        if x.endswith( "dftables.c" ):
            return False
        if x.endswith( "pcredemo.c" ):
            return False
        if x.endswith( "pcretest.c" ):
            return False
        if x.endswith( "unittest.cc" ):
            return False
        if x.endswith( "pcregrep.c" ):
            return False
        return x.endswith( ".c" ) or x.endswith( ".cc" )

    files = [ root + "/" + x for x in filter( pcreFilter , os.listdir( root ) ) ]
    
    return files

def configure( env , fileLists , options ):
    #fileLists = { "serverOnlyFiles" : [] }

    env.Prepend( CPPPATH=["./third_party/pcre-7.4/"] )

    myenv = env.Clone()
    myenv.Append( CPPDEFINES=["HAVE_CONFIG_H"] )
    fileLists["commonFiles"] += [ myenv.Object(f) for f in getFiles() ]



if __name__ == "__main__":
    for x in getFiles():
        print( x )
