import gdb
from collections import defaultdict
#import blessings

str_t_info = {0: 'int32s',
              1: 'uint8s',
              2: 'rope',
              3: 'mask'}

class MVMStringPPrinter(object):
    def __init__(self, val, pointer = False):
        self.val = val
        self.pointer = pointer

    def stringify(self):
        stringtyp = str_t_info[int(self.val['body']['flags']) & 0b11]
        if stringtyp in ("int32s", "uint8s"):
            zero_reached = False
            data = self.val['body'][stringtyp]
            i = 0
            pieces = []
            while not zero_reached:
                pdata = int((data + i).dereference())
                if pdata == 0:
                    zero_reached = True
                else:
                    pieces.append(chr(pdata))
                i += 1
            return "".join(pieces)
        elif stringtyp == "rope":
            i = 0
            pieces = []
            data = self.val['body']['strands']
            end_reached = False
            previous_index = 0
            previous_string = None
            while not end_reached:
                strand_data = (data + i).dereference()
                if strand_data['string'] == 0:
                    end_reached = True
                    pieces.append(previous_string[1:-1])
                else:
                    the_string = strand_data['string'].dereference()
                    if previous_string is not None:
                        pieces.append(
                            str(previous_string)[1:-1][
                                int(strand_data['string_offset']) :
                                int(strand_data['compare_offset']) - previous_index]
                            )
                    previous_string = str(the_string)
                    previous_index = int(strand_data['compare_offset'])
                i = i + 1
            return "r(" + ")(".join(pieces) + ")"
        else:
            return "string of type " + stringtyp

    def to_string(self):
        if self.pointer:
            return "pointer to '" + self.stringify() + "'"
        else:
            return "'" + self.stringify() + "'"

# currently nonfunctional
class MVMObjectPPrinter(object):
    def __init__(self, val, pointer = False):
        self.val = val
        self.pointer = pointer

    def stringify(self):
        if self.pointer:
            as_mvmobject = self.val.cast("MVMObject *").dereference()
        else:
            as_mvmobject = self.val.cast("MVMObject")

        _repr = as_mvmobject['st']['REPR']
        
        reprname = _repr['name'].string()
        
        return str(self.val.type.name) + " of repr " + reprname

    def to_string(self):
        if self.pointer:
            return "pointer to " + self.stringify()
        else:
            return self.stringify()

sizes_histogram = defaultdict(lambda: 0)
reprs_histogram = defaultdict(lambda: 0)

reprs_gen2_histogram = defaultdict(lambda: 0)
reprs_nursery_killed_histogram = defaultdict(lambda: 0)

reprs_nursery_seen = defaultdict(lambda: 0)
reprs_gen2_seen = defaultdict(lambda: 0)

class StartAnalyzeGC(gdb.Command):
    def __init__(self):
         super (StartAnalyzeGC, self).__init__ ("start-analyze-gc", gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        # set a breakpoint for gc_run
        gdb.write("please type 'end' into the prompt now")
        gdb.execute("break orchestrate.c:run_gc")
        gdb.execute("commands")
        gdb.execute("run-gc-triggered")
        gdb.execute("continue")
        gdb.execute("end")

class RunGCTriggered(gdb.Command):
    def __init__(self):
        super (RunGCTriggered, self).__init__("run-gc-triggered", gdb.COMMAND_DATA) # XXX invisible command

    def invoke(self, arg, from_tty):
        if from_tty:
            gdb.write("the run-gc-triggered command should only be run by the breakpoint set up by the start-analyze-gc command!")
        for i in range(20):
            gdb.execute("step")
            frame = gdb.newest_frame()
            print frame.name()


def str_lookup_function(val):
    if str(val.type) == "MVMString":
        return MVMStringPPrinter(val)
    elif str(val.type) == "MVMString *":
        return MVMStringPPrinter(val, True)

    return None

def mvmobject_lookup_function(val):
    return None
    pointer = str(val.type).endswith("*")
    if str(val.type).startswith("MVM"):
        try:
            val.cast(gdb.lookup_type("MVMObject" + (" *" if pointer else "")))
            return MVMObjectPPrinter(val, pointer)
        except Exception as e:
            print "couldn't cast this:", e
            pass
    return None

def register_printers(objfile):
    objfile.pretty_printers.append(str_lookup_function)
    print "MoarVM string pretty printer registered"
    objfile.pretty_printers.append(mvmobject_lookup_function)
    print "MoarVM Object pretty printer registered"

commands = []
def register_commands(objfile):
    commands.append(StartAnalyzeGC())
    commands.append(RunGCTriggered())

if __name__ == "__main__":
    register_printers(gdb.current_objfile())
    register_commands(gdb.current_objfile())
