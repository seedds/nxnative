// swift-tools-version: 6.0
import PackageDescription
let package = Package(name:"NXMotion",platforms:[.macOS(.v13)],products:[.library(name:"NXMotion",targets:["NXMotion"])],targets:[.target(name:"NXMotion",path:".",exclude:["Package.swift","Tests"],resources:[.process("DSUMotionTranslations.json")]),.testTarget(name:"NXMotionTests",dependencies:["NXMotion"],path:"Tests")],swiftLanguageModes:[.v5])
