require 'yaml'
require 'fileutils'

PGSQL_ENV = 'connection1'
PGSQL_INF = YAML.load_file(File.join(File.dirname(__FILE__), '..', 'server', 'database.yml'))[PGSQL_ENV]
PGSQL_DATA = File.join(File.dirname(__FILE__), '..', 'server', 'data')

def exec_command cmd
  case cmd
  when Array
    cmd.each{ |c| exec_command c }
  else
    puts cmd
    unless system(cmd)
      raise "Error executing cmd: #{cmd}: #{$?}"
    end
  end
end

def server_running? path = PGSQL_DATA
  pg_data = File.expand_path(path)
  pidfile = File.join(pg_data, 'postmaster.pid')
  (FileTest.directory?(pg_data) &&
   FileTest.exist?(pidfile) &&
   FileTest.directory?("/proc/#{File.readlines(pidfile).first.chomp}"))
end

def server_start path = PGSQL_DATA
  pg_data = File.expand_path(path)

  return if server_running? path
  return unless PGSQL_INF['host'] == 'localhost'
  exec_command [%[PGHOST=#{PGSQL_INF['host']} PGPORT=#{PGSQL_INF['port']} pg_ctl start -w -D "#{pg_data}"]]
end

def server_stop path = PGSQL_DATA
  pg_data = File.expand_path(path)

  return unless server_running? pg_data
  return unless PGSQL_INF['host'] == 'localhost'
  exec_command %[PGHOST=#{PGSQL_INF['host']} PGPORT=#{PGSQL_INF['port']} pg_ctl stop -D "#{pg_data}"]
end

def server_build path = PGSQL_DATA
  pg_data = File.expand_path(path)

  return unless PGSQL_INF['host'] == 'localhost'
  server_clean pg_data

  exec_command %[initdb --no-locale -D "#{pg_data}"]

  File.open(File.join(pg_data, 'postgresql.conf'), 'a') do |out|
    out.puts "listen_addresses = '#{PGSQL_INF['host']}'"
    out.puts "port = #{PGSQL_INF['port']}"
    out.puts "unix_socket_directory = '#{pg_data}'"
    out.puts "log_destination = 'stderr'"
    out.puts "logging_collector = on"
    out.puts "log_directory = 'pg_log'"
    out.puts "log_line_prefix = '%t:%r:%u@%d:[%p]: '"
    out.puts "log_statement = 'all'"
  end

  exec_command [%[PGHOST=#{PGSQL_INF['host']} PGPORT=#{PGSQL_INF['port']} pg_ctl start -w -D "#{pg_data}"],
                %[createdb -h #{PGSQL_INF['host']} -p #{PGSQL_INF['port']} #{PGSQL_INF['dbname']}]]
end

def server_clean path = PGSQL_DATA
  pg_data = File.expand_path(path)

  return unless PGSQL_INF['host'] == 'localhost'
  server_stop pg_data

  FileUtils.rm_rf pg_data
end

if __FILE__ == $PROGRAM_NAME
  puts server_running?
  server_stop
  server_clean
  server_build
  server_start
  server_stop
  puts server_running?
end
