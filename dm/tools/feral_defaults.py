"""Set build defaults in Feral's specifications.xml before a game creates its preferences."""
from xml.etree import ElementTree as ET

PATH = 'resources/specifications.xml'


def apply(data, settings):
    try:
        root = ET.fromstring(data, parser=ET.XMLParser(target=ET.TreeBuilder(insert_comments=True)))
    except ET.ParseError as error:
        raise ValueError(f'invalid Feral specifications: {error}') from error
    if root.tag != 'gameinfo':
        raise ValueError('expected a Feral gameinfo document')
    default_specs = [spec for spec in root.findall('spec')
                     if not any(isinstance(node.tag, str) for node in spec.findall('conditions/*'))]
    changed = False
    for path, name, value in settings:
        matches = [entry for spec in default_specs for key in spec.findall('regkey') if key.get('path') == path
                   for entry in key.findall('value') if entry.get('name') == name]
        if len(matches) != 1 or matches[0].get('type') != 'integer' or len(matches[0]):
            raise ValueError(f'expected one unconditional integer default for {path} / {name}')
        value = str(int(value))
        if matches[0].text != value:
            matches[0].text = value
            changed = True
    return ET.tostring(root, encoding='utf-8', xml_declaration=True) if changed else data
