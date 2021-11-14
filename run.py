
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time
from typing import Any, Dict, Iterable, List, Optional, Tuple, Union
from uuid import UUID, uuid4
import zipfile


class gem5Run:
    """
    This class holds all of the info required to run gem5.
    """

    _id: UUID
    hash: str
    type: str
    name: str
    gem5_binary: Path
    run_script: Path
    params: Tuple[str, ...]
    timeout: int

    gem5_name: str
    script_name: str
    linux_name: str
    disk_name: str
    string: str

    outdir: Path

    linux_binary: Path
    disk_image: Path


    command: List[str]

    running: bool
    enqueue_time: float
    start_time: float
    end_time: float
    return_code: int
    kill_reason: str
    status: str
    pid: int
    task_id: Any

    @classmethod
    def _create(cls,
                name: str,
                gem5_binary: Path,
                run_script: Path,
                outdir: Path,
                params: Tuple[str, ...],
                timeout: int) -> 'gem5Run':
        """
        Shared code between SE and FS when creating a run object.
        """
        run = cls()
        run.name = name
        run.gem5_binary = gem5_binary
        run.run_script = run_script
        run.params = params
        run.timeout = timeout

        run._id = uuid4()

        run.outdir = outdir.resolve() # ensure this is absolute

        # Assumes **/<gem5_name>/gem5.<anything>
        run.gem5_name = run.gem5_binary.parent.name
        # Assumes **/<script_name>.py
        run.script_name = run.run_script.stem

        # Info about the actual run
        run.running = False
        run.enqueue_time = time.time()
        run.start_time = 0.0
        run.end_time = 0.0
        run.return_code = 0
        run.kill_reason = ''
        run.status = "Created"
        run.pid = 0
        run.task_id = None

        # Initially, there are no results
        run.results = None

        return run

    @classmethod
    def createSERun(cls,
                    name: str,
                    gem5_binary: str,
                    run_script: str,
                    outdir: str,
                    *params: str,
                    timeout: int = 60*15) -> 'gem5Run':
        """
        name is the name of the run. The name is not necessarily unique. The
        name could be used to query the results of the run.
        gem5_binary and run_script are the paths to the binary to run
        and the script to pass to gem5. Full paths are better.
        The artifact parameters (gem5_artifact, gem5_git_artifact, and
        run_script_git_artifact) are used to ensure this is reproducible run.
        Further parameters can be passed via extra arguments. These
        parameters will be passed in order to the gem5 run script.
        timeout is the time in seconds to run the subprocess before killing it.
        Note: When instantiating this class for the first time, it will create
        a file `info.json` in the outdir which contains a serialized version
        of this class.
        """

        run = cls._create(name, Path(gem5_binary), Path(run_script),
                          Path(outdir), params, timeout)

        run.string = f"{run.gem5_name} {run.script_name}"
        run.string += ' '.join(run.params)

        run.command = [
            str(run.gem5_binary),
            '-re', f'--outdir={run.outdir}',
            str(run.run_script)]
        run.command += list(params)

        run.hash = run._getHash()
        run.type = 'gem5 run'

        # Make the directory if it doesn't exist
        os.makedirs(run.outdir, exist_ok=True)
        run.dumpJson('info.json')

        return run


    @classmethod
    def loadJson(cls, filename: str) -> 'gem5Run':
        with open(filename) as f:
            d = json.load(f)
            # Convert string version of UUID to UUID object
            for k,v in d.iteritems():
                if k.endswith('_artifact'):
                    d[k] = UUID(v)
            d['_id'] = UUID(d['_id'])
        try:
            return cls.loadFromDict(d)
        except KeyError:
            print("Incompatible json file: {}!".format(filename))
            raise

    @classmethod
    def loadFromDict(cls, d: Dict[str, Union[str, UUID]]) -> 'gem5Run':
        """Returns new gem5Run instance from the dictionary of values in d"""
        run = cls()
        for k,v in d.items():
            setattr(run, k, v)
        return run

    def __repr__(self) -> str:
        return str(self._getSerializable())

    def checkKernelPanic(self) -> bool:
        """
        Returns true if the gem5 instance specified in args has a kernel panic
        Note: this gets around the problem that gem5 doesn't exit on panics.
        """
        term_path = self.outdir / 'system.pc.com_1.device'
        if not term_path.exists():
            return False

        with open(term_path, 'rb') as f:
            try:
                f.seek(-1000, os.SEEK_END)
            except OSError:
                return False
            last = f.readlines()[-1].decode()
            if 'Kernel panic' in last:
                return True
            else:
                return False

    def _getSerializable(self) -> Dict[str, Union[str, UUID]]:
        """Returns a dictionary that can be used to recreate this object
        Note: All artifacts are converted to a UUID instead of an Artifact.
        """
        # Grab all of the member variables
        d = vars(self).copy()

        # Replace the artifacts with their UUIDs
        for k,v in d.items():
            if isinstance(v, Path):
                d[k] = str(v)

        return d

    def _getHash(self) -> str:
        """Return a single value that uniquely identifies this run
        To uniquely identify this run, the gem5 binary, gem5 scripts, and
        parameters should all match. Thus, let's make a single hash out of the
        artifacts + the runscript + parameters
        """
        to_hash = []
        to_hash.append(str(self.run_script).encode())
        to_hash.append(' '.join(self.params).encode())

        return hashlib.md5(b''.join(to_hash)).hexdigest()

    @classmethod
    def _convertForJson(cls, d: Dict[str, Any]) -> Dict[str, str]:
        """Converts UUID objects to strings for json compatibility"""
        for k,v in d.items():
            if isinstance(v, UUID):
                d[k] = str(v)
        return d

    def dumpJson(self, filename: str) -> None:
        """Dump all info into a json file"""
        d = self._convertForJson(self._getSerializable())
        with open(self.outdir / filename, 'w') as f:
            json.dump(d, f)

    def dumpsJson(self) -> str:
        """Like dumpJson except returns string"""
        d = self._convertForJson(self._getSerializable())
        return json.dumps(d)

    def run(self, task: Any = None, cwd: str = '.') -> None:
        """Actually run the test.
        Calls Popen with the command to fork a new process.
        Then, this function polls the process every 5 seconds to check if it
        has finished or not. Each time it checks, it dumps the json info so
        other applications can poll those files.
        task is the celery task that is running this gem5 instance.
        cwd is the directory to change to before running. This allows a server
        process to run in a different directory than the running process. Note
        that only the spawned process runs in the new directory.
        """
        # Check if the run is already in the database
        self.status = "Begin run"
        self.dumpJson('info.json')

        self.status = "Spawning"

        self.start_time = time.time()

        self.dumpJson('info.json')

        # Start running the gem5 command
        proc = subprocess.Popen(self.command, cwd = cwd)

        # Register handler in case this process is killed while the gem5
        # instance is running. Note: there's a bit of a race condition here,
        # but hopefully it's not a big deal
        def handler(signum, frame):
            proc.kill()
            self.kill_reason = 'sigterm'
            self.dumpJson('info.json')
            # Note: We'll fall out of the while loop after this.

        # This makes it so if you term *this* process, it will actually kill
        # the subprocess and then this process will die.
        signal.signal(signal.SIGTERM, handler)

        # Do this until the subprocess is done (successfully or not)
        while proc.poll() is None:
            self.status = "Running"
            # Still running
            self.current_time = time.time()
            self.pid = proc.pid
            self.running = True

            if self.current_time - self.start_time > self.timeout:
                proc.kill()
                self.kill_reason = 'timeout'

            if self.checkKernelPanic():
                proc.kill()
                self.kill_reason = 'kernel panic'

            self.dumpJson('info.json')

            # Check again in five seconds
            time.sleep(5)

        print("Done running {}".format(' '.join(self.command)))

        # Done executing
        self.running = False
        self.end_time = time.time()
        self.return_code = proc.returncode

        if self.return_code == 0:
            self.status = "Finished"
        else:
            self.status = "Failed"

        self.dumpJson('info.json')

        self.saveResults()

        # Store current gem5 run in the database

        print("Done storing the results of {}".format(' '.join(self.command)))

    def saveResults(self) -> None:
        """Zip up the output directory and store the results in the database.
        """

        with zipfile.ZipFile(self.outdir / 'results.zip', 'w',
                             zipfile.ZIP_DEFLATED) as zipf:
            for path in self.outdir.glob("**/*"):
                if path.name == 'results.zip': continue
                zipf.write(path, path.relative_to(self.outdir.parent))

    def __str__(self) -> str:
        return  self.string + ' -> ' + self.status

