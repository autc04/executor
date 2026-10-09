#### Expected Failures
# Test cases that test some aspect of the Mac API that Executor does not currently support,
# but which are not cause for immediate concern.

set_tests_properties(
            # get info on directories doesn't work yet
        FileTest.GetFInfo

            # creation dates not supported by linux filesystem
        FileTest.SetFInfo_CrDat 

            # unimplemented
        FileTest.SetFLock 

            # MakeFSSpec should resolve current directory, Executor stores 0 in FSSpec
        FileTest.MakeFSSpec

            # LocalVolume::PBCatMove resolves ioNewName as the new parent
            # directory instead of the new item name, so move-with-rename fails.
        LocalVolumeFixture.MoveAndRename
    APPEND PROPERTIES LABELS xfail)
