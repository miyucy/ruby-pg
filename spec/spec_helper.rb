require 'yaml'
require 'fileutils'

PGSQL_ENV = 'connection1'
PGSQL_INF = YAML.load_file(File.join(File.dirname(__FILE__), '..', 'server', 'database.yml'))[PGSQL_ENV]
PGSQL_DATA = File.join(File.dirname(__FILE__), '..', 'server', 'data')

def exec_commands cmds
  cmds.each do |cmd|
    puts cmd
    unless system(cmd)
      raise "Error executing cmd: #{cmd}: #{$?}"
    end
  end
end

def server_running? path = PGSQL_DATA
  pg_data = File.expand_path(path)
  (test(?e, File.join(pg_data, 'postmaster.pid')) &&
   system(%[ps #{File.readlines(File.join(pg_data, 'postmaster.pid')).first}]))
end

def server_start path = PGSQL_DATA
  return if server_running? path
  return unless PGSQL_INF['host'] == 'localhost'
  pg_data = File.expand_path(path)
  exec_commands [%[PGHOST=#{PGSQL_INF['host']} PGPORT=#{PGSQL_INF['port']} pg_ctl start -w -D "#{pg_data}"]]
end

def server_stop path = PGSQL_DATA
  return unless server_running? path
  return unless PGSQL_INF['host'] == 'localhost'
  pg_data = File.expand_path(path)
  exec_commands [%[PGHOST=#{PGSQL_INF['host']} PGPORT=#{PGSQL_INF['port']} pg_ctl stop -D "#{pg_data}"]]
end

def server_build path = PGSQL_DATA
  server_stop path if server_running? path
  return unless PGSQL_INF['host'] == 'localhost'
  pg_data = File.expand_path(path)

  exec_commands [%[initdb --no-locale -D "#{pg_data}"]] unless test(?d, pg_data)

  File.open(File.join(path, 'postgresql.conf'), 'a') do |out|
    out.puts "listen_addresses = #{PGSQL_INF['host']}"
    out.puts "port = #{PGSQL_INF['port']}"
    out.puts "unix_socket_directory = '#{File.dirname(pg_data)}'"
    out.puts "log_destination = 'stderr'"
    out.puts "logging_collector = on"
    out.puts "log_directory = 'pg_log'"
    out.puts "log_line_prefix = '%t:%r:%u@%d:[%p]: '"
    out.puts "log_statement = 'all'"
  end

  exec_commands [%[PGHOST=#{PGSQL_INF['host']} PGPORT=#{PGSQL_INF['port']} pg_ctl start -w -D "#{pg_data}"]]
  unless system %[psql -h #{PGSQL_INF['host']} -p #{PGSQL_INF['port']} -l -t | awk -F '|' '{print $1}' | grep #{PGSQL_INF['dbname']}]
    exec_commands [%[createdb -h #{PGSQL_INF['host']} -p #{PGSQL_INF['port']} #{PGSQL_INF['dbname']}]]
  end
end

def server_clean path = PGSQL_DATA
  server_stop path if server_running? path
  return unless PGSQL_INF['host'] == 'localhost'
  pg_data = File.expand_path(path)
  exec_commands [%[rm -rf #{pg_data}]]
end

if __FILE__ == $PROGRAM_NAME
  puts server_running?
end
