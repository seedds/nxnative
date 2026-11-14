"""Real strict private-PKI verifier with temporary, original certificates."""
from pathlib import Path
import subprocess
import sys
import tempfile

def main():
    binary, openssl = map(str, (Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()))
    with tempfile.TemporaryDirectory(prefix='nx-private-pki-') as folder:
        root = Path(folder)
        def command(*arguments):
            subprocess.run([openssl,*arguments],cwd=root,capture_output=True,check=True)
        (root/'ca.cnf').write_text('[req]\ndistinguished_name=dn\nx509_extensions=ca\nprompt=no\n[dn]\nCN=Original ephemeral NX test authority\n[ca]\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid:always\n')
        command('req','-x509','-newkey','rsa:2048','-nodes','-keyout','ca.key','-out','ca.pem','-days','10000','-config','ca.cnf')
        command('x509','-in','ca.pem','-outform','der','-out','ca.der')
        command('req','-new','-newkey','rsa:2048','-nodes','-keyout','leaf.key','-out','leaf.csr','-subj','/CN=api.service.invalid')
        # The system OpenSSL/LibreSSL CLI supports explicit validity through
        # `ca`, including versions whose `x509` command cannot set these dates.
        (root/'issued').mkdir()
        (root/'index.txt').touch()
        (root/'serial.txt').write_text('1000\n')
        (root/'issuer.cnf').write_text('[ca]\ndefault_ca=issuer\n[issuer]\n'
            'database=index.txt\nnew_certs_dir=issued\nserial=serial.txt\n'
            'certificate=ca.pem\nprivate_key=ca.key\ndefault_md=sha256\n'
            'policy=policy\nunique_subject=no\n[policy]\ncommonName=supplied\n')
        def dated(csr,output,extra,key=None,extensions=None):
            dates=dict(zip(extra[::2],extra[1::2]))
            arguments=['ca','-batch','-notext','-config','issuer.cnf','-in',csr,'-out',output,
                       '-startdate',dates['-not_before'],'-enddate',dates['-not_after']]
            if key: arguments += ['-selfsign','-keyfile',key,'-cert','pin.pem']
            if extensions: arguments += ['-extfile','leaf.cnf','-extensions',extensions]
            command(*arguments)
        def leaf(name,san='DNS:api.service.invalid',eku='serverAuth',extra=()):
            (root/'leaf.cnf').write_text('[leaf]\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid,issuer\nextendedKeyUsage='+eku+'\n'+('subjectAltName='+san+'\n' if san else ''))
            if extra:
                dated('leaf.csr',name+'.pem',extra,extensions='leaf')
            else:
                command('x509','-req','-in','leaf.csr','-CA','ca.pem','-CAkey','ca.key','-set_serial',str(10+len(list(root.glob('*.der')))),
                        '-out',name+'.pem','-days','3650','-extfile','leaf.cnf','-extensions','leaf')
            command('x509','-in',name+'.pem','-outform','der','-out',name+'.der')
        leaf('valid'); leaf('cn-only',san=''); leaf('client',eku='clientAuth')
        leaf('expired',extra=('-not_before','20200101000000Z','-not_after','20200102000000Z'))
        leaf('future',extra=('-not_before','20350101000000Z','-not_after','20360102000000Z'))
        leaf('wildcard',san='DNS:*.service.invalid'); leaf('partial',san='DNS:api*.service.invalid')
        (root/'trailing.der').write_bytes((root/'valid.der').read_bytes()+b'\0')
        (root/'broken.der').write_bytes(b'not a certificate')
        cases=[('api.service.invalid','ca.der','valid.der',True),('other.service.invalid','ca.der','valid.der',False),
               ('api.service.invalid','ca.der','cn-only.der',False),('api.service.invalid','ca.der','client.der',False),
               ('api.service.invalid','ca.der','expired.der',False),('api.service.invalid','ca.der','future.der',False),
               ('api.service.invalid','ca.der','wildcard.der',True),('nested.api.service.invalid','ca.der','wildcard.der',False),
               ('api.service.invalid','ca.der','partial.der',False),('api.service.invalid','valid.der','valid.der',False),
               ('api.service.invalid','broken.der','valid.der',False),('api.service.invalid','ca.der','broken.der',False),
               ('api.service.invalid','ca.der','trailing.der',False),('api.service.invalid','trailing.der','valid.der',False),
               ('127.0.0.1','ca.der','valid.der',False),('bad..service.invalid','ca.der','valid.der',False)]
        for host,anchor,certificate,accepted in cases:
            result=subprocess.run([binary,host,str(root/anchor),str(root/certificate)],capture_output=True)
            if result.returncode != (0 if accepted else 1): raise AssertionError((host,anchor,certificate,result.returncode,result.stderr))
        print('Strict private X.509:',len(cases),'cases passed; 10-year private leaf accepted, SAN/time/purpose/trust/DER failures rejected.')
        def pin(name, key='leaf.key', extra=(), extensions=None):
            csr = 'leaf.csr'
            if key != 'leaf.key':
                csr = name+'.csr'
                command('req','-new','-key',key,'-out',csr,'-subj','/CN=api.service.invalid')
            arguments = ['x509','-req','-in',csr,'-signkey',key,'-out',name+'.pem','-days','1',*extra]
            if extensions:
                (root/'pin.cnf').write_text(extensions)
                arguments += ['-extfile','pin.cnf']
            if '-not_before' in extra: dated(csr,name+'.pem',extra,key)
            else: command(*arguments)
            command('x509','-in',name+'.pem','-outform','der','-out',name+'.der')
        pin('pin'); pin('rotated')
        pin('pin-expired',extra=('-not_before','20200101000000Z','-not_after','20200102000000Z'))
        pin('pin-future',extra=('-not_before','20350101000000Z','-not_after','20360102000000Z'))
        pin('pin-client',extensions='basicConstraints=CA:FALSE\nextendedKeyUsage=clientAuth\n')
        pin('pin-ca',extensions='basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign\n')
        pin('pin-authority-endpoint',extensions='basicConstraints=critical,CA:TRUE\n')
        pin('pin-sha1',extra=('-sha1',))
        command('genrsa','-out','weak.key','1024'); pin('pin-weak',key='weak.key')
        (root/'pin-trailing.der').write_bytes((root/'pin.der').read_bytes()+b'\0')
        damaged = bytearray((root/'pin.der').read_bytes()); damaged[-1] ^= 1
        (root/'pin-damaged.der').write_bytes(damaged)
        pin_cases=[('api.service.invalid','pin.der','pin.der',True),
                   ('api.service.invalid','pin.der','rotated.der',False),
                   ('api.service.invalid','ca.der','pin.der',False),
                   ('api.service.invalid','pin-expired.der','pin-expired.der',False),
                   ('api.service.invalid','pin-future.der','pin-future.der',False),
                   ('api.service.invalid','pin-client.der','pin-client.der',False),
                   ('api.service.invalid','pin-ca.der','pin-ca.der',False),
                   ('api.service.invalid','pin-authority-endpoint.der','pin-authority-endpoint.der',True),
                   ('api.service.invalid','ca.der','valid.der',False),
                   ('api.service.invalid','pin-sha1.der','pin-sha1.der',False),
                   ('api.service.invalid','pin-weak.der','pin-weak.der',False),
                   ('api.service.invalid','pin-trailing.der','pin-trailing.der',False),
                   ('api.service.invalid','pin-damaged.der','pin-damaged.der',False),
                   ('bad..service.invalid','pin.der','pin.der',False)]
        for host,enrolled,certificate,accepted in pin_cases:
            result=subprocess.run([binary,'--pin',host,str(root/enrolled),str(root/certificate)],capture_output=True)
            if result.returncode != (0 if accepted else 1): raise AssertionError(('pin',enrolled,certificate,result.returncode,result.stderr))
        print('Explicit leaf enrollment:',len(pin_cases),'cases passed; exact DER, time, purpose, signature and key strength enforced.')

if __name__=='__main__':main()
