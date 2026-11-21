use nxformat_packer::*;
use nxformat_packer::native::synthetic::*;
use std::{fs,path::PathBuf};
const APP:u64=0x0100000000010000;
fn npdm()->Vec<u8>{
 let mut b=vec![0;0x400]; b[..4].copy_from_slice(b"META");
 b[0x70..0x74].copy_from_slice(&0x80u32.to_le_bytes());b[0x74..0x78].copy_from_slice(&0x80u32.to_le_bytes());
 b[0x80..0x84].copy_from_slice(b"ACI0");b[0x90..0x98].copy_from_slice(&APP.to_le_bytes());b
}
fn file(n:&str,b:Vec<u8>)->EntryInput{EntryInput::file(n,FileInput::from_bytes(b))}
fn tree(marker:&str)->DirectoryInput{DirectoryInput::new(vec![
 EntryInput::directory("exefs",DirectoryInput::new(vec![file("main",vec![b'M';128]),file("main.npdm",npdm())])),
 EntryInput::directory("romfs",DirectoryInput::new(vec![file(marker,vec![b'A';150000]),file("empty",vec![]),EntryInput::directory("empty-dir",DirectoryInput::empty())]))])}
fn snap(kind:SnapshotType,version:u32)->SnapshotInput{SnapshotInput{addon_title_id:None,native:Default::default(),program_id:Some(APP),snapshot_type:kind,numeric_version:version,content_id:None,build_id:None,display_version:None,dependencies:vec![],metadata:if kind==SnapshotType::Dlc{None}else{Some(SnapshotMetadata{title:Some(format!("Synthetic {version}")),publisher:None,icon:Some(MediaInput::from_bytes(MediaType::JpegIcon,vec![0xff,0xd8,0xff,0xe0,if version==0{0}else{1}])),nacp:Some(MediaInput::from_bytes(MediaType::Nacp,vec![0;0x4000])),localized:vec![],opaque:vec![]})},root:tree(if version==0{"base.txt"}else{"update.txt"})}}
fn main()->Result<(),Box<dyn std::error::Error>>{
 let out=PathBuf::from(std::env::args().nth(1).unwrap());fs::create_dir_all(&out)?;
 for (name,platform) in [("switch1",Platform::Switch1),("switch2",Platform::Switch2)]{
  for (mode,compression_mode) in [("raw",CompressionMode::RawOnly),("zstd",CompressionMode::ZstandardAuto)]{
   let mut dlc=snap(SnapshotType::Dlc,0);dlc.program_id=None;dlc.addon_title_id=Some(APP+0x1001);dlc.root=DirectoryInput::new(vec![EntryInput::directory("romfs",DirectoryInput::new(vec![file("dlc.txt",b"synthetic DLC".to_vec())]))]);
   write_archive(ArchiveInput::new(vec![ApplicationInput{application_id:APP,platform,programs:vec![ProgramInput{program_id:APP,kind:ProgramKind::Primary,content_id:None,npdm:None}],snapshots:vec![snap(SnapshotType::Base,0),snap(SnapshotType::Update,65536),dlc]}]),out.join(format!("{name}-{mode}.nx")),&WriterOptions{compression_mode,chunk_size_bytes:65536,compressed_chunk_bytes:65536,..Default::default()})?;
  }
  fn native_snapshot(spec:&TitleSpec)->SnapshotInput {
   let entries=title_entries(spec);let meta=entries.iter().find(|(name,_)|name.ends_with(".cnmt.nca")).unwrap();
   let mut content=[0u8;16];for i in 0..16{content[i]=u8::from_str_radix(&meta.0[i*2..i*2+2],16).unwrap();}
   let kind=match spec.meta_type{0x80=>SnapshotType::Base,0x81=>SnapshotType::Update,_=>SnapshotType::Dlc};
   SnapshotInput{addon_title_id:None,native:Default::default(),program_id:if kind==SnapshotType::Dlc{None}else{Some(APP)},snapshot_type:kind,numeric_version:spec.version,content_id:Some(content),build_id:None,display_version:None,dependencies:vec![],metadata:None,root:DirectoryInput::new(entries.into_iter().map(|(n,b)|file(&n,b)).collect())}
  }
  fn native_input(platform:Platform,snapshots:Vec<SnapshotInput>)->ArchiveInput{ArchiveInput::new(vec![ApplicationInput{application_id:APP,platform,programs:vec![ProgramInput{program_id:APP,kind:ProgramKind::Primary,content_id:None,npdm:None}],snapshots}])}
  let mut specs=vec![TitleSpec::application(APP,0,1),TitleSpec::patch(APP,65536,2),TitleSpec::add_on(APP,APP+0x1001,0,3)];
  specs[2].required_application_version=65536;
  for spec in &mut specs {spec.platform=if name=="switch1"{0}else{1};if let Some(p)=&mut spec.program{p.bktr=false;p.npdm=Some(npdm());}}
  let temp=out.join("base.tmp.nx");write_archive(native_input(platform,vec![native_snapshot(&specs[0])]),&temp,&WriterOptions::default())?;
  let identity=inspect_archive(&temp,&WriterOptions::default(),IntegrityPolicy::Strict)?.applications[0].snapshots[0].identity;fs::remove_file(temp)?;
  let base=native_snapshot(&specs[0]);let mut update=native_snapshot(&specs[1]);update.dependencies.push(DependencyInput{kind:DependencyKind::RequiredBase,content_id:None,target_snapshot_identity:identity});
  let mut addon=native_snapshot(&specs[2]);addon.dependencies.push(DependencyInput{kind:DependencyKind::Requires,content_id:None,target_snapshot_identity:identity});
  let mut missing=native_snapshot(&specs[1]);missing.dependencies.push(DependencyInput{kind:DependencyKind::RequiredBase,content_id:None,target_snapshot_identity:identity});
  write_archive(native_input(platform,vec![missing]),out.join(format!("{name}-missing-base.nx")),&WriterOptions::default())?;
  write_archive(native_input(platform,vec![base,update,addon]),out.join(format!("{name}-native.nx")),&WriterOptions::default())?;

 }
 Ok(())
}
