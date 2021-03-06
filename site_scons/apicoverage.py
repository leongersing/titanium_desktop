#
# Titanium API Coverage Generator
#
# Initial Author: Jeff Haynie, 3/30/09
#
import glob, re, os.path as path
import fnmatch, os, sys
import simplejson as json
import traceback

class GlobDirectoryWalker:
	# a forward iterator that traverses a directory tree

	def __init__(self, directory, patterns=['*']):
		self.stack = [directory]
		self.patterns = patterns
		self.files = []
		self.index = 0

	def __getitem__(self, index):
		while 1:
			try:
				file = self.files[self.index]
				self.index = self.index + 1
			except IndexError:
				# pop next directory from stack
				self.directory = self.stack.pop()
				self.files = os.listdir(self.directory)
				self.index = 0
			else:
				# got a filename
				fullname = os.path.join(self.directory, file)
				if os.path.isdir(fullname) and not os.path.islink(fullname):
					self.stack.append(fullname)
				for pattern in self.patterns:
					if fnmatch.fnmatch(file, pattern):
						return fullname

def convert_type(value):
	# trivial type conversions
	if type(value)!=str:
		return value
	if value == 'True' or value == 'true':
		return True
	elif value == 'False' or value == 'false':
		return False
	elif re.match('^[0-9]+$',value):
		return int(value)
	elif re.match('^[0-9\.]+$',value):
		return float(value)
	return value

def parse_key_value_pairs(pairs, metadata):
	for kvpair in pairs.strip().split(','):
		key, value = kvpair.split('=')
		metadata[key.strip()] = value.strip()
	return metadata

def get_property(h,name,default,convert=True):
	if not name in h:
		return default
	
	try:
		if convert:
			return convert_type(h[name])
		else:
			return h[name]
	except:
		return default
		

class Module(object):
	def __init__(self, name):
		self.name = name
		self.api_points = []
		self.api_points_map = {}

	def add_api(self, api):
		if api.name in self.api_points_map.keys():
			raise Exception("Tried to add %s API twice!" % api.name)
		else:
			self.api_points.append(api)
			self.api_points_map[api.name] = api
			api.module = self
	
	def get_api_with_name(self, api_name):
		if not api_name in self.api_points_map.keys():
			raise Exception("Tried to modify %s API before defining it!" % api.name)
		else:
			return self.api_points_map[api_name]
	
	@staticmethod
	def get_with_name(name):
		if not(name in Module.modules.keys()):
			Module.modules[name] = Module(name)
		return Module.modules[name]
	
	@staticmethod
	def all_as_dict():
		d = {}
		for m in Module.modules.values():
			d[m.name] = m.api_points_map
		return d

class API(dict):
	@staticmethod
	def create_with_full_name(fullName):
		module = None
		api = None
		if fullName.find(".") == -1:
			module = Module.get_with_name('<global>')
			api = API(fullName, module)
		else:
			module_name, api_name = fullName.strip().split('.', 1)
			module = Module.get_with_name(module_name)
			api = API(api_name, module)
		
		module.add_api(api)
		print "adding %s -- %s" % (api.module.name, api.name)
		return api
	
	@staticmethod
	def get_with_full_name(fullName):
		module_name, api_name = fullName.strip().split('.', 1)
		module = Module.get_with_name(module_name)
		api = module.get_api_with_name(api_name)
		return api
	
	def __init__(self, name, module=None):
		API.count += 1
		self.name = self['name'] = name
		self.module = module
		self['deprecated'] = False
		self['since'] = '0.3'
		self['description'] = ''
	
	def add_metadata(self, metadata):
		self.name = get_property(metadata, 'name', self.name)
		self['deprecated'] = get_property(metadata, 'deprecated', self['deprecated'])
		self['description'] = get_property(metadata, 'description', self['description'])
		self['since'] = get_property(metadata, 'since', self['since'], convert=False)
		if get_property(metadata, 'method', False):
			self['method'] = True
			self['returns'] = None
			self['arguments'] = []
		if get_property(metadata, 'property', False):
			self['property'] = True
	
	def __str__(self):
		return 'API<%s>' % self.name
	
	def add_argument(self,arg):
		try:
			self['arguments'].append(arg)
		except:
			print "Invalid type: %s" % self
		
	def set_return_type(self,return_type):
		self['returns'] = return_type
	
	def set_deprecated(self,msg,version):
		self.deprecated = True
		self['deprecated'] = msg
		self['deprecated_on'] = version

class APIArgument(dict):
	def __init__(self, params, description):
		self['description'] = description
		self['name'] = params['name']
		self.forname = params['for']
		self['type'] = get_property(params,'type','object')
		self['optional'] = get_property(params,'optional',False)
	
	def __str__(self):
		return 'APIArgument<%s>' % self['name']

class APIReturnType(dict):
	def __init__(self, params, description):
		self.forname = params['for']
		self['description'] = description
		self['type'] = get_property(params,'type','void')
	
	def __str__(self):
		return 'APIReturnType<%s>' % self['name']

def get_last_method_before(method_index, start):
	current_start = None
	
	method_starts = method_index.keys()
	method_starts.sort()
	for method_start in method_starts:
		if method_start > start:
			break
		else:
			current_start = method_start
	
	if current_start:
		return method_index[current_start]
	else:
		return None

def generate_api_coverage(dirs,fs):
	API.count = 0
	Module.modules = {}

	api_pattern = '@tiapi\(([^\)]*)\)(.*)'
	arg_pattern = '@tiarg\(([^\)]*)\)(.*)'
	res_pattern = '@tiresult\(([^\)]*)\)(.*)'
	dep_pattern = '@tideprecated\(([^\)]*)\)(.*)'

	context_sensitive_arg_pattern = '@tiarg\[([^\]]+)\](.*)'
	context_sensitive_result_pattern = '@tiresult\[([^\]]+)\](.*)'
	context_sensitive_api_description = '@tiapi (.*)'
	context_sensitive_arg_description = '@tiarg (.*)'
	context_sensitive_result_description = '@tiresult (.*)'
	tiproperty_pattern = '@tiproperty\[([^\]]+)\](.*)'

	files = set()
	files_with_matches = set()

	extensions = ['h','cc','c','cpp','m','mm','js','py','rb']
	extensions = ['*.' + x for x in extensions]
	for dirname in dirs:
		print dirname
		for i in GlobDirectoryWalker(dirname, extensions):
			files.add(i)

	for filename in files:
		current_api = None
		current_arg = None
		current_result = None
		match = None
		for line in open(filename,'r').read().splitlines():
			try:
				m = re.search(api_pattern, line)
				if m is not None:
					files_with_matches.add(filename)
					metadata = parse_key_value_pairs(m.group(1).strip(), {})
					metadata['description'] = m.group(2).strip()
					api = API.create_with_full_name(metadata['name'])
					api.add_metadata(metadata)
					current_api = api
					continue
			
				m = re.search(tiproperty_pattern, line)
				if m is not None:
					files_with_matches.add(filename)
					bits = m.group(1).split(',', 2)
					metadata = {}
					metadata['type'] = bits[0]
					metadata['description'] = m.group(2).strip()
					if len(bits) > 2:
						metadata = parse_key_value_pairs(bits[2], metadata)
					api = API.create_with_full_name(bits[1])
					api.add_metadata(metadata)
					current_api = api
					continue
			
				m = re.search(context_sensitive_arg_pattern, line)
				if m is not None:
					files_with_matches.add(filename)
					if not current_api: continue
					bits = m.group(1).split(',', 2)
					metadata = {}
					metadata['for'] = current_api.name
					metadata['type'] = bits[0].strip()
					metadata['name'] = bits[1].strip()
					metadata['description'] = m.group(2).strip()
					if len(bits) > 2:
						metadata = parse_key_value_pairs(bits[2], metadata)
					current_arg = APIArgument(metadata, metadata['description'])
					current_api.add_argument(current_arg)
					continue
			
				m = re.search(context_sensitive_result_pattern, line)
				if m is not None:
					files_with_matches.add(filename)
					if not current_api: continue
					metadata = {}
					metadata['type'] = m.group(1).strip()
					metadata['description'] = m.group(2).strip()
					metadata['for'] = current_api.name
					current_result = APIReturnType(metadata, metadata['description'])
					current_api.set_return_type(current_result)
					continue

				m = re.search(context_sensitive_api_description, line)
				if m is not None:
					files_with_matches.add(filename)
					description = m.group(1)
					if current_api:
						description = current_api['description'] + ' ' + description.strip()
						current_api['description'] = description.strip()
						continue

				m = re.search(context_sensitive_arg_description, line)
				if m is not None:
					files_with_matches.add(filename)
					description = m.group(1)
					if current_arg:
						description = current_arg['description'] + ' ' + description.strip()
						current_arg['description'] = description.strip()
						continue

				m = re.search(context_sensitive_result_description, line)
				if m is not None:
					files_with_matches.add(filename)
					description = m.group(1)
					if current_result:
						description = current_result['description'] + ' ' + description.strip()
						current_result['description'] = description.strip()
						continue
			
				m = re.search(arg_pattern, line)
				if m is not None:
					files_with_matches.add(filename)
					description = m.group(2).strip()
					metadata = parse_key_value_pairs(m.group(1).strip(), {})
					api = API.get_with_full_name(metadata['for'])
					api.add_argument(APIArgument(metadata, description))
					continue
			
				m = re.search(res_pattern, line)
				if m is not None:
					files_with_matches.add(filename)
					description = m.group(2).strip()
					metadata = parse_key_value_pairs(m.group(1).strip(), {})
					api = API.get_with_full_name(metadata['for'])
					api.set_return_type(APIReturnType(metadata, description))
					continue
			
				m = re.search(dep_pattern, line)
				if m is not None:
					files_with_matches.add(filename)
					description = m.group(2).strip()
					metadata = parse_key_value_pairs(m.group(1).strip(), {})
					api = API.get_with_full_name(metadata['for'])
					api.set_deprecated(description, metadata['version'])
			except Exception, e:
				print"Exception parsing API metadata in file: %s" % filename
				raise

	fs.write(json.dumps(Module.all_as_dict(), sort_keys=True, indent=4))

	print "Found %i APIs for %i modules in %i files" % (API.count, len(Module.modules), len(files_with_matches))

if __name__ == '__main__':
	if len(sys.argv)!=3:
		print "Usage: %s <dir> <outfile>" % os.path.basename(sys.argv[0])
		sys.exit(1)
	f = open(os.path.expanduser(sys.argv[2]), 'w')
	dirs = []
	dirs.append(os.path.abspath(os.path.expanduser(sys.argv[1])))
	generate_api_coverage(dirs,f)	
	
